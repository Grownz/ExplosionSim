#include "gfx/Renderer.h"

#include "core/Log.h"
#include "gfx/Shader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace ex::gfx {

namespace {

constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr int kParticleStride = sizeof(sim::ParticleInstance);

float SmoothNoiseWeight(float t) {
    return t * t * (3.0f - 2.0f * t);
}

float LatticeHash(int x, int y, int z, int seed) {
    uint32_t hash = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u +
                    uint32_t(z) * 2246822519u + uint32_t(seed) * 3266489917u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    hash ^= hash >> 16;
    return float(hash & 0xFFFFFFu) / 16777215.0f;
}

float PeriodicNoise(float x, float y, float z, int period, int seed) {
    const int xi = int(std::floor(x));
    const int yi = int(std::floor(y));
    const int zi = int(std::floor(z));
    const float fx = SmoothNoiseWeight(x - float(xi));
    const float fy = SmoothNoiseWeight(y - float(yi));
    const float fz = SmoothNoiseWeight(z - float(zi));

    auto wrap = [period](int value) {
        const int result = value % period;
        return result < 0 ? result + period : result;
    };

    const int x0 = wrap(xi);
    const int y0 = wrap(yi);
    const int z0 = wrap(zi);
    const int x1 = wrap(xi + 1);
    const int y1 = wrap(yi + 1);
    const int z1 = wrap(zi + 1);

    const float n000 = LatticeHash(x0, y0, z0, seed);
    const float n100 = LatticeHash(x1, y0, z0, seed);
    const float n010 = LatticeHash(x0, y1, z0, seed);
    const float n110 = LatticeHash(x1, y1, z0, seed);
    const float n001 = LatticeHash(x0, y0, z1, seed);
    const float n101 = LatticeHash(x1, y0, z1, seed);
    const float n011 = LatticeHash(x0, y1, z1, seed);
    const float n111 = LatticeHash(x1, y1, z1, seed);

    const float nx00 = n000 + (n100 - n000) * fx;
    const float nx10 = n010 + (n110 - n010) * fx;
    const float nx01 = n001 + (n101 - n001) * fx;
    const float nx11 = n011 + (n111 - n011) * fx;
    const float nxy0 = nx00 + (nx10 - nx00) * fy;
    const float nxy1 = nx01 + (nx11 - nx01) * fy;
    return nxy0 + (nxy1 - nxy0) * fz;
}

float TileableFbm(float u, float v, float w, int basePeriod, int octaves, int seed) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float normalization = 0.0f;
    int period = basePeriod;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude *
               PeriodicNoise(u * float(period), v * float(period), w * float(period), period,
                             seed + i * 17);
        normalization += amplitude;
        amplitude *= 0.5f;
        period *= 2;
    }
    return sum / std::max(normalization, 1e-6f);
}

bool CreateStructuredBuffer(ID3D11Device* device, int capacity, ComPtr<ID3D11Buffer>& buffer,
                            ComPtr<ID3D11ShaderResourceView>& srv) {
    buffer.Reset();
    srv.Reset();
    if (capacity <= 0) {
        return true;
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(capacity) * kParticleStride;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = kParticleStride;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = UINT(capacity);
    return SUCCEEDED(device->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv));
}

} // namespace

bool Renderer::Initialize(GfxDevice& device) {
    device_ = &device;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(FrameConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->Device()->CreateBuffer(&cbDesc, nullptr, &frameConstantBuffer_))) {
        LogError("Failed to create frame constant buffer.");
        return false;
    }

    cbDesc.ByteWidth = sizeof(BloomConstants);
    if (FAILED(device_->Device()->CreateBuffer(&cbDesc, nullptr, &bloomConstantBuffer_))) {
        LogError("Failed to create bloom constant buffer.");
        return false;
    }

    cbDesc.ByteWidth = sizeof(ShadowConstants);
    if (FAILED(device_->Device()->CreateBuffer(&cbDesc, nullptr,
                                               &shadowConstantBuffer_))) {
        LogError("Failed to create shadow constant buffer.");
        return false;
    }

    if (!CreateStates() || !CreatePipeline() || !CreateNoiseTexture() ||
        !EnsureTargets(device_->Width(), device_->Height())) {
        return false;
    }

    if (!solver_.Initialize(device_->Device(), device_->Context(),
                            device_->DedicatedVideoMemory())) {
        LogWarn("High-fidelity GPU solver unavailable.");
    }
    if (!sceneRenderer_.Initialize(device_->Device())) {
        LogWarn("Procedural scene renderer unavailable.");
    }
    return true;
}

void Renderer::Shutdown() {
    sceneRenderer_.Shutdown();
    solver_.Shutdown();
    shadowConstantBuffer_.Reset();
    volumeSRV_.Reset();
    volumeTexture_.Reset();
    particleSRVAdditive_.Reset();
    particleSRVAlpha_.Reset();
    particleSRVFire_.Reset();
    particleBufferAdditive_.Reset();
    particleBufferAlpha_.Reset();
    particleBufferFire_.Reset();
    hdr_.Reset();
    for (auto& target : bloom_) {
        target.Reset();
    }
    device_ = nullptr;
}

bool Renderer::CreateStates() {
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->Device()->CreateSamplerState(&samplerDesc, &linearClamp_))) {
        return false;
    }

    const UINT anisoLevels[3] = {2, 4, 8};
    for (int i = 0; i < 3; ++i) {
        D3D11_SAMPLER_DESC anisoDesc = samplerDesc;
        anisoDesc.Filter = D3D11_FILTER_ANISOTROPIC;
        anisoDesc.MaxAnisotropy = anisoLevels[i];
        if (FAILED(device_->Device()->CreateSamplerState(&anisoDesc, &anisoClamp_[i]))) {
            return false;
        }
    }

    D3D11_SAMPLER_DESC wrapDesc = samplerDesc;
    wrapDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    wrapDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (FAILED(device_->Device()->CreateSamplerState(&wrapDesc, &linearWrap_))) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        D3D11_SAMPLER_DESC anisoDesc = wrapDesc;
        anisoDesc.Filter = D3D11_FILTER_ANISOTROPIC;
        anisoDesc.MaxAnisotropy = anisoLevels[i];
        if (FAILED(device_->Device()->CreateSamplerState(&anisoDesc, &anisoWrap_[i]))) {
            return false;
        }
    }

    auto createBlend = [this](D3D11_BLEND source, D3D11_BLEND destination,
                              ComPtr<ID3D11BlendState>& state) {
        D3D11_BLEND_DESC desc{};
        desc.RenderTarget[0].BlendEnable = TRUE;
        desc.RenderTarget[0].SrcBlend = source;
        desc.RenderTarget[0].DestBlend = destination;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(device_->Device()->CreateBlendState(&desc, &state));
    };

    if (!createBlend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, blendOpaque_) ||
        !createBlend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, blendAlpha_) ||
        !createBlend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, blendAdditive_) ||
        !createBlend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, blendPremultiplied_)) {
        LogError("Failed to create blend states.");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    depthDesc.StencilEnable = FALSE;
    if (FAILED(device_->Device()->CreateDepthStencilState(&depthDesc, &depthDisabled_))) {
        return false;
    }

    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device_->Device()->CreateDepthStencilState(&depthDesc, &depthTest_))) {
        return false;
    }

    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    if (FAILED(device_->Device()->CreateDepthStencilState(&depthDesc, &depthWrite_))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device_->Device()->CreateRasterizerState(&rasterizerDesc, &rasterizer_))) {
        LogError("Failed to create the rasterizer state.");
        return false;
    }

    return true;
}

bool Renderer::CreatePipeline() {
    auto compile = [](const wchar_t* relativePath, const char* entry, const char* profile) {
        const std::wstring path = ResolveAssetPath(relativePath);
        if (path.empty()) {
            LogError("Shader file not found: %ls", relativePath);
            return ComPtr<ID3DBlob>{};
        }
        std::string errors;
        return CompileShaderFile(path, entry, profile, errors);
    };

    const wchar_t* skyFile = L"assets\\shaders\\sky.hlsl";
    const wchar_t* groundFile = L"assets\\shaders\\ground.hlsl";
    const wchar_t* volumeFile = L"assets\\shaders\\volume.hlsl";
    const wchar_t* shockFile = L"assets\\shaders\\shock.hlsl";
    const wchar_t* particlesFile = L"assets\\shaders\\particles.hlsl";
    const wchar_t* bloomFile = L"assets\\shaders\\bloom.hlsl";
    const wchar_t* compositeFile = L"assets\\shaders\\composite.hlsl";

    auto vsBlob = compile(skyFile, "FullscreenVS", "vs_5_0");
    auto skyBlob = compile(skyFile, "PSSky", "ps_5_0");
    auto groundBlob = compile(groundFile, "PSGround", "ps_5_0");
    auto volumeBlob = compile(volumeFile, "PSVolume", "ps_5_0");
    auto shockBlob = compile(shockFile, "PSShock", "ps_5_0");
    auto particleVSBlob = compile(particlesFile, "VSParticle", "vs_5_0");
    auto particleAddBlob = compile(particlesFile, "PSAdditive", "ps_5_0");
    auto particleAlphaBlob = compile(particlesFile, "PSAlpha", "ps_5_0");
    auto particleFireBlob = compile(particlesFile, "PSFire", "ps_5_0");
    auto downsampleBlob = compile(bloomFile, "PSDownsample", "ps_5_0");
    auto upsampleBlob = compile(bloomFile, "PSUpsample", "ps_5_0");
    auto compositeBlob = compile(compositeFile, "PSComposite", "ps_5_0");
    auto volumeBakeBlob =
        compile(L"assets\\shaders\\volume_bake.hlsl", "CSBake", "cs_5_0");
    auto particleGpuBlob =
        compile(L"assets\\shaders\\particles_gpu.hlsl", "CSIntegrate", "cs_5_0");
    auto particleSpawnBlob =
        compile(L"assets\\shaders\\particles_gpu.hlsl", "CSApplySpawns", "cs_5_0");
    auto particleFinalizeBlob =
        compile(L"assets\\shaders\\particles_gpu.hlsl", "CSFinalize", "cs_5_0");
    auto particleResetBlob =
        compile(L"assets\\shaders\\particles_gpu.hlsl", "CSReset", "cs_5_0");

    if (!vsBlob || !skyBlob || !groundBlob || !volumeBlob || !shockBlob ||
        !particleVSBlob || !particleAddBlob || !particleAlphaBlob ||
        !particleFireBlob || !downsampleBlob || !upsampleBlob || !compositeBlob) {
        LogError("One or more shaders failed to compile.");
        return false;
    }

    ID3D11Device* device = device_->Device();
    auto createVS = [device](ID3DBlob* blob, ComPtr<ID3D11VertexShader>& shader) {
        return blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(),
                                                            blob->GetBufferSize(), nullptr, &shader));
    };
    auto createPS = [device](ID3DBlob* blob, ComPtr<ID3D11PixelShader>& shader) {
        return blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(),
                                                           blob->GetBufferSize(), nullptr, &shader));
    };

    if (!createVS(vsBlob.Get(), fullscreenVS_) || !createPS(skyBlob.Get(), skyPS_) ||
        !createPS(groundBlob.Get(), groundPS_) || !createPS(volumeBlob.Get(), volumePS_) ||
        !createPS(shockBlob.Get(), shockPS_) ||
        !createVS(particleVSBlob.Get(), particleVS_) ||
        !createPS(particleAddBlob.Get(), particleAdditivePS_) ||
        !createPS(particleAlphaBlob.Get(), particleAlphaPS_) ||
        !createPS(particleFireBlob.Get(), particleFirePS_) ||
        !createPS(downsampleBlob.Get(), downsamplePS_) ||
        !createPS(upsampleBlob.Get(), upsamplePS_) ||
        !createPS(compositeBlob.Get(), compositePS_)) {
        LogError("Failed to create shader objects.");
        return false;
    }

    // Optional: the GPU volume bake. A compile failure leaves the CPU baker in
    // charge instead of taking the whole renderer down.
    if (volumeBakeBlob &&
        SUCCEEDED(device->CreateComputeShader(volumeBakeBlob->GetBufferPointer(),
                                              volumeBakeBlob->GetBufferSize(), nullptr,
                                              &volumeBakeCS_))) {
        D3D11_BUFFER_DESC bakeDesc{};
        bakeDesc.ByteWidth = sizeof(VolumeBakeConstants);
        bakeDesc.Usage = D3D11_USAGE_DYNAMIC;
        bakeDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bakeDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bakeDesc, nullptr, &volumeBakeConstantBuffer_))) {
            LogWarn("GPU volume bake: constant buffer creation failed; using the CPU bake.");
            volumeBakeCS_.Reset();
        }
    } else {
        LogWarn("GPU volume bake shader unavailable; using the CPU bake.");
    }

    // Optional: GPU particles. As with the volume bake, a failure leaves the
    // CPU path in charge.
    const bool particleShaders =
        particleGpuBlob && particleSpawnBlob && particleFinalizeBlob &&
        particleResetBlob &&
        SUCCEEDED(device->CreateComputeShader(particleGpuBlob->GetBufferPointer(),
                                              particleGpuBlob->GetBufferSize(), nullptr,
                                              &particleIntegrateCS_)) &&
        SUCCEEDED(device->CreateComputeShader(particleSpawnBlob->GetBufferPointer(),
                                              particleSpawnBlob->GetBufferSize(), nullptr,
                                              &particleSpawnCS_)) &&
        SUCCEEDED(device->CreateComputeShader(particleFinalizeBlob->GetBufferPointer(),
                                              particleFinalizeBlob->GetBufferSize(), nullptr,
                                              &particleFinalizeCS_)) &&
        SUCCEEDED(device->CreateComputeShader(particleResetBlob->GetBufferPointer(),
                                              particleResetBlob->GetBufferSize(), nullptr,
                                              &particleResetCS_));
    if (particleShaders) {
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(ParticleUpdateConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &particleUpdateCB_))) {
            LogWarn("GPU particles: constant buffer creation failed; using the CPU path.");
            particleSpawnCS_.Reset();
            particleIntegrateCS_.Reset();
            particleFinalizeCS_.Reset();
            particleFinalizeCS_.Reset();
        }
    } else {
        LogWarn("GPU particle shader unavailable; using the CPU path.");
    }

    return true;
}

bool Renderer::CreateNoiseTexture() {
    constexpr int kSize = 64;
    std::vector<uint8_t> data(size_t(kSize) * kSize * kSize * 4);

    for (int z = 0; z < kSize; ++z) {
        const float w = (float(z) + 0.5f) / float(kSize);
        for (int y = 0; y < kSize; ++y) {
            const float v = (float(y) + 0.5f) / float(kSize);
            for (int x = 0; x < kSize; ++x) {
                const float u = (float(x) + 0.5f) / float(kSize);
                const size_t index = (size_t(z) * kSize * kSize + size_t(y) * kSize + size_t(x)) * 4;
                data[index + 0] = uint8_t(std::clamp(TileableFbm(u, v, w, 4, 4, 11), 0.0f, 1.0f) * 255.0f);
                data[index + 1] = uint8_t(std::clamp(TileableFbm(u, v, w, 5, 4, 23), 0.0f, 1.0f) * 255.0f);
                data[index + 2] = uint8_t(std::clamp(TileableFbm(u, v, w, 6, 4, 37), 0.0f, 1.0f) * 255.0f);
                data[index + 3] = uint8_t(std::clamp(TileableFbm(u, v, w, 10, 5, 53), 0.0f, 1.0f) * 255.0f);
            }
        }
    }

    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = kSize;
    desc.Height = kSize;
    desc.Depth = kSize;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data.data();
    initial.SysMemPitch = UINT(kSize) * 4;
    initial.SysMemSlicePitch = UINT(kSize) * UINT(kSize) * 4;

    if (FAILED(device_->Device()->CreateTexture3D(&desc, &initial, &noiseTexture_)) ||
        FAILED(device_->Device()->CreateShaderResourceView(noiseTexture_.Get(), nullptr,
                                                           &noiseSRV_))) {
        LogError("Failed to create the detail noise texture.");
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->Device()->CreateSamplerState(&samplerDesc, &noiseSampler_))) {
        LogError("Failed to create the detail noise sampler.");
        return false;
    }

    // 1x1x1 placeholder bound to the auxiliary volume slot when the volume is
    // not split into two RG8 planes.
    D3D11_TEXTURE3D_DESC dummyDesc{};
    dummyDesc.Width = 1;
    dummyDesc.Height = 1;
    dummyDesc.Depth = 1;
    dummyDesc.MipLevels = 1;
    dummyDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dummyDesc.Usage = D3D11_USAGE_DEFAULT;
    dummyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint8_t zero[4] = {0, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA dummyData{};
    dummyData.pSysMem = zero;
    dummyData.SysMemPitch = 4;
    dummyData.SysMemSlicePitch = 4;
    ComPtr<ID3D11Texture3D> dummyTexture;
    if (FAILED(device_->Device()->CreateTexture3D(&dummyDesc, &dummyData, &dummyTexture)) ||
        FAILED(device_->Device()->CreateShaderResourceView(dummyTexture.Get(), nullptr,
                                                           &volumeDummySRV_))) {
        LogError("Failed to create the dummy volume texture.");
        return false;
    }
    return true;
}

bool Renderer::CreateTarget(Target& target, int width, int height) {
    target.Reset();
    width = std::max(1, width);
    height = std::max(1, height);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width);
    desc.Height = UINT(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kHdrFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device_->Device()->CreateTexture2D(&desc, nullptr, &target.texture)) ||
        FAILED(device_->Device()->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv)) ||
        FAILED(device_->Device()->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv))) {
        LogError("Failed to create render target %dx%d.", width, height);
        target.Reset();
        return false;
    }

    target.width = width;
    target.height = height;
    return true;
}

bool Renderer::EnsureTargets(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (hdr_.width == width && hdr_.height == height && hdr_.texture) {
        return true;
    }

    if (!CreateTarget(hdr_, width, height)) {
        return false;
    }
    if (!CreateTarget(hdrCopy_, width, height)) {
        return false;
    }

    int bloomWidth = width;
    int bloomHeight = height;
    for (int i = 0; i < kBloomLevels; ++i) {
        bloomWidth = std::max(1, bloomWidth / 2);
        bloomHeight = std::max(1, bloomHeight / 2);
        if (!CreateTarget(bloom_[i], bloomWidth, bloomHeight)) {
            return false;
        }
    }

    depthCopyTexture_.Reset();
    depthCopySRV_.Reset();
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(width);
        desc.Height = UINT(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device_->Device()->CreateTexture2D(&desc, nullptr, &depthCopyTexture_)) ||
            FAILED(device_->Device()->CreateShaderResourceView(depthCopyTexture_.Get(), nullptr,
                                                               &depthCopySRV_))) {
            LogError("Failed to create the depth copy texture.");
            return false;
        }
    }

    return true;
}

void Renderer::Resize(int width, int height) {
    EnsureTargets(width, height);
}

bool Renderer::EnsureVolumeTexture(int resolution, bool halfFloat, bool split) {
    if (volumeTexture_ && volumeResolution_ == resolution &&
        volumeHalfFloat_ == halfFloat && volumeSplit_ == split) {
        return true;
    }

    volumeSRV_.Reset();
    volumeUAV_.Reset();
    volumeTexture_.Reset();
    volumeAuxSRV_.Reset();
    volumeAuxTexture_.Reset();

    const DXGI_FORMAT format =
        split ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;

    auto createVolume = [this](int res, DXGI_FORMAT textureFormat, bool withUav,
                               ComPtr<ID3D11Texture3D>& texture,
                               ComPtr<ID3D11ShaderResourceView>& srv,
                               ComPtr<ID3D11UnorderedAccessView>* uav) {
        D3D11_TEXTURE3D_DESC desc{};
        desc.Width = UINT(res);
        desc.Height = UINT(res);
        desc.Depth = UINT(res);
        desc.MipLevels = 1;
        desc.Format = textureFormat;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                         (withUav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        if (FAILED(device_->Device()->CreateTexture3D(&desc, nullptr, &texture))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = textureFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Texture3D.MipLevels = 1;
        if (FAILED(device_->Device()->CreateShaderResourceView(texture.Get(), &srvDesc,
                                                               &srv))) {
            return false;
        }
        if (withUav && uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = textureFormat;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            uavDesc.Texture3D.MipSlice = 0;
            uavDesc.Texture3D.FirstWSlice = 0;
            uavDesc.Texture3D.WSize = UINT(res);
            if (FAILED(device_->Device()->CreateUnorderedAccessView(
                    texture.Get(), &uavDesc, uav->GetAddressOf()))) {
                return false;
            }
        }
        return true;
    };

    if (!createVolume(resolution, format, !split, volumeTexture_, volumeSRV_,
                      &volumeUAV_)) {
        LogError("Failed to create 3D volume texture at %d^3.", resolution);
        volumeValid_ = false;
        return false;
    }

    if (split) {
        if (!createVolume(resolution, DXGI_FORMAT_R8G8_UNORM, false,
                          volumeAuxTexture_, volumeAuxSRV_, nullptr)) {
            LogError("Failed to create the auxiliary 3D volume texture at %d^3.",
                     resolution);
            volumeValid_ = false;
            return false;
        }
    } else {
        volumeAuxSRV_ = volumeDummySRV_;
    }

    volumeResolution_ = resolution;
    volumeHalfFloat_ = halfFloat;
    volumeSplit_ = split;
    return true;
}

void Renderer::UploadVolume(const sim::VolumeFrame& frame) {
    if (frame.empty()) {
        volumeValid_ = false;
        return;
    }

    if (!EnsureVolumeTexture(frame.resolution, frame.halfFloat, frame.split)) {
        return;
    }

    const UINT resolution = UINT(frame.resolution);
    const UINT rowPitch = resolution * UINT(frame.bytesPerVoxel);
    const UINT depthPitch = rowPitch * resolution;
    device_->Context()->UpdateSubresource(volumeTexture_.Get(), 0, nullptr, frame.rgba.data(),
                                          rowPitch, depthPitch);

    if (frame.split && volumeAuxTexture_) {
        const UINT auxPitch = resolution * 2;
        const UINT auxDepthPitch = auxPitch * resolution;
        device_->Context()->UpdateSubresource(volumeAuxTexture_.Get(), 0, nullptr,
                                              frame.rgba2.data(), auxPitch, auxDepthPitch);
    }

    volumeValid_ = true;
}

bool Renderer::DispatchGpuVolumeBake(const sim::VolumeBakeRequest& request) {
    if (!volumeBakeCS_ || !volumeBakeConstantBuffer_ || !request.valid() ||
        request.split) {
        return false;
    }
    if (!EnsureVolumeTexture(request.resolution, true, false) || !volumeUAV_) {
        return false;
    }

    const sim::VolumeFieldParams& p = request.params;
    VolumeBakeConstants constants{};
    constants.boundsMin =
        XMFLOAT4(request.bounds.min.x, request.bounds.min.y, request.bounds.min.z,
                 float(request.resolution));
    constants.boundsSize =
        XMFLOAT4(request.bounds.max.x - request.bounds.min.x,
                 request.bounds.max.y - request.bounds.min.y,
                 request.bounds.max.z - request.bounds.min.z, request.voxelSize);
    constants.fireball =
        XMFLOAT4(p.fireballX, p.fireballY, p.fireballZ, p.fireballRadius);
    constants.fireball2 = XMFLOAT4(p.fireballTemperature, p.fireballVisible ? 1.0f : 0.0f,
                                   p.time, p.ambientTemperature);
    constants.cloud = XMFLOAT4(p.cloudX, p.cloudY, p.cloudZ, p.cloudRadius);
    constants.cloud2 =
        XMFLOAT4(p.cloudTemperature, p.stemRadius, p.cloudStrength, p.dustRingRadius);
    constants.shock =
        XMFLOAT4(p.shockRadius, p.shockMach, p.burstY, p.dustIntensity);

    ID3D11DeviceContext* context = device_->Context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(volumeBakeConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                            0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(volumeBakeConstantBuffer_.Get(), 0);

    // The texture may still be bound as a pixel-shader resource from the
    // previous frame; D3D11 refuses a UAV bind while the SRV is active.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
    context->CSSetShaderResources(0, 1, &nullSRV);

    context->CSSetShader(volumeBakeCS_.Get(), nullptr, 0);
    ID3D11Buffer* cbuffer = volumeBakeConstantBuffer_.Get();
    context->CSSetConstantBuffers(0, 1, &cbuffer);
    ID3D11UnorderedAccessView* uav = volumeUAV_.Get();
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

    const int groups = (request.resolution + 3) / 4;
    context->Dispatch(groups, groups, groups);

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);

    gpuVolumeBounds_ = request.bounds;
    gpuVolumeExtinction_ = request.extinctionScale;
    gpuVolumeBakeActive_ = true;
    volumeValid_ = true;
    return true;
}

void Renderer::UploadParticles(const std::vector<sim::ParticleInstance>& instances,
                               ComPtr<ID3D11Buffer>& buffer) {
    if (instances.empty() || !buffer) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ID3D11DeviceContext* context = device_->Context();
    if (FAILED(context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    memcpy(mapped.pData, instances.data(), instances.size() * kParticleStride);
    context->Unmap(buffer.Get(), 0);
}

bool Renderer::CreateParticleGpuResources(int capacity) {
    if (capacity <= 0 || !particleSpawnCS_ || !particleIntegrateCS_ ||
        !particleFinalizeCS_ || !particleUpdateCB_) {
        return false;
    }

    particleStateBuffer_.Reset();
    particleStateUAV_.Reset();
    particleSpawnBuffer_.Reset();
    particleSpawnSRV_.Reset();
    particleCounterBuffer_.Reset();
    particleCounterUAV_.Reset();
    particleIndirectBuffer_.Reset();
    particleIndirectUAV_.Reset();
    particleCounterStaging_.Reset();
    particleReadbackQuery_.Reset();
    particleUAVAdditive_.Reset();
    particleUAVAlpha_.Reset();
    particleUAVFire_.Reset();
    particleReadbackPending_ = false;

    ID3D11Device* device = device_->Device();

    auto createBuffer = [device](UINT bytes, UINT stride, D3D11_USAGE usage, UINT bind,
                                 UINT cpuAccess, UINT misc, ComPtr<ID3D11Buffer>& buffer) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.Usage = usage;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = cpuAccess;
        desc.MiscFlags = misc | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = stride;
        return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer));
    };
    auto createUav = [device](ID3D11Buffer* buffer, UINT elements,
                              ComPtr<ID3D11UnorderedAccessView>& uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = elements;
        return SUCCEEDED(device->CreateUnorderedAccessView(buffer, &desc, &uav));
    };
    auto createSrv = [device](ID3D11Buffer* buffer, UINT elements,
                              ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = elements;
        return SUCCEEDED(device->CreateShaderResourceView(buffer, &desc, &srv));
    };

    const UINT stateStride = UINT(sizeof(sim::ParticleGpuState));
    const UINT spawnStride = UINT(sizeof(sim::ParticleSpawn));
    if (!createBuffer(UINT(capacity) * stateStride, stateStride, D3D11_USAGE_DEFAULT,
                      D3D11_BIND_UNORDERED_ACCESS, 0, 0, particleStateBuffer_) ||
        !createUav(particleStateBuffer_.Get(), UINT(capacity), particleStateUAV_)) {
        LogError("GPU particles: state buffer creation failed (%d elements).", capacity);
        return false;
    }
    if (!createBuffer(UINT(capacity) * spawnStride, spawnStride, D3D11_USAGE_DYNAMIC,
                      D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0,
                      particleSpawnBuffer_) ||
        !createSrv(particleSpawnBuffer_.Get(), UINT(capacity), particleSpawnSRV_)) {
        LogError("GPU particles: spawn buffer creation failed (%d elements).", capacity);
        return false;
    }
    if (!createBuffer(4 * sizeof(uint32_t), sizeof(uint32_t), D3D11_USAGE_DEFAULT,
                      D3D11_BIND_UNORDERED_ACCESS, 0, 0, particleCounterBuffer_) ||
        !createUav(particleCounterBuffer_.Get(), 4, particleCounterUAV_)) {
        LogError("GPU particles: counter buffer creation failed.");
        return false;
    }
    // Indirect args must be a raw buffer; a structured buffer cannot carry
    // DRAWINDIRECT_ARGS.
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 12 * sizeof(uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &particleIndirectBuffer_))) {
            LogError("GPU particles: indirect args buffer creation failed.");
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 12;
        if (FAILED(device->CreateUnorderedAccessView(particleIndirectBuffer_.Get(),
                                                     &uavDesc, &particleIndirectUAV_))) {
            LogError("GPU particles: indirect args UAV creation failed.");
            return false;
        }
    }
    if (!createBuffer(4 * sizeof(uint32_t), sizeof(uint32_t), D3D11_USAGE_STAGING, 0,
                      D3D11_CPU_ACCESS_READ, 0, particleCounterStaging_)) {
        LogError("GPU particles: counter staging buffer creation failed.");
        return false;
    }

    D3D11_QUERY_DESC queryDesc{};
    queryDesc.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateQuery(&queryDesc, &particleReadbackQuery_))) {
        LogError("GPU particles: readback query creation failed.");
        return false;
    }

    // Instance buffers for the GPU path: the compute shader writes them and the
    // vertex shader reads them.
    auto createInstanceBuffer = [&](ComPtr<ID3D11Buffer>& buffer,
                                    ComPtr<ID3D11ShaderResourceView>& srv,
                                    ComPtr<ID3D11UnorderedAccessView>& uav) {
        if (!createBuffer(UINT(capacity) * kParticleStride, kParticleStride,
                          D3D11_USAGE_DEFAULT,
                          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0,
                          buffer) ||
            !createSrv(buffer.Get(), UINT(capacity), srv) ||
            !createUav(buffer.Get(), UINT(capacity), uav)) {
            return false;
        }
        return true;
    };
    if (!createInstanceBuffer(particleBufferAdditive_, particleSRVAdditive_,
                              particleUAVAdditive_) ||
        !createInstanceBuffer(particleBufferAlpha_, particleSRVAlpha_, particleUAVAlpha_) ||
        !createInstanceBuffer(particleBufferFire_, particleSRVFire_, particleUAVFire_)) {
        LogError("GPU particles: instance buffer creation failed.");
        return false;
    }

    ID3D11DeviceContext* context = device_->Context();
    const UINT zeros[4] = {0, 0, 0, 0};
    context->ClearUnorderedAccessViewUint(particleCounterUAV_.Get(), zeros);
    return true;
}

void Renderer::StepGpuParticles(sim::Simulation& simulation) {
    sim::ParticleSystem& particles = simulation.Particles();
    if (!particles.GpuMode() || !SupportsGpuParticles()) {
        return;
    }
    const int capacity = particles.Capacity();
    if (capacity != particleCapacity_ || !particleGpuMode_) {
        if (!CreateParticleGpuResources(capacity)) {
            particles.SetGpuMode(false);
            return;
        }
        particleCapacity_ = capacity;
        particleGpuMode_ = true;
    }
    UpdateGpuParticles(simulation);
}

void Renderer::UpdateGpuParticles(sim::Simulation& simulation) {
    sim::ParticleSystem& particles = simulation.Particles();    if (particleReadbackPending_) {
        ReadParticleCounters(particles);
    }

    ID3D11DeviceContext* context = device_->Context();
    const bool reset = particles.ConsumeGpuReset();
    const float dt = particles.ConsumeGpuDt();
    if (!reset && dt <= 0.0f) {
        return;
    }
    const UINT zeros[4] = {0, 0, 0, 0};

    const std::vector<sim::ParticleSpawn>& spawns = particles.PrepareGpuSpawns();
    if (!spawns.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(particleSpawnBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                   &mapped))) {
            memcpy(mapped.pData, spawns.data(),
                   spawns.size() * sizeof(sim::ParticleSpawn));
            context->Unmap(particleSpawnBuffer_.Get(), 0);
        }
    }

    const sim::ParticleGpuParams& gp = particles.GpuParams();
    ParticleUpdateConstants constants{};
    constants.windTime = XMFLOAT4(gp.wind.x, gp.wind.y, gp.wind.z, gp.time);
    constants.params = XMFLOAT4(gp.ambientTemperature, gp.gravity, dt,
                                float(particleCapacity_));    constants.counts = XMFLOAT4(float(spawns.size()), 0.0f, 0.0f, 0.0f);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(particleUpdateCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                            &mapped))) {
        return;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(particleUpdateCB_.Get(), 0);

    // Clear the counters for this frame.
    context->ClearUnorderedAccessViewUint(particleCounterUAV_.Get(), zeros);

    // A structured-buffer UAV clear only writes the first 16 bytes of each
    // element, so the state reset needs a real pass.
    if (reset) {
        ID3D11Buffer* cbuffer = particleUpdateCB_.Get();
        context->CSSetConstantBuffers(0, 1, &cbuffer);
        ID3D11UnorderedAccessView* stateUav = particleStateUAV_.Get();
        context->CSSetUnorderedAccessViews(0, 1, &stateUav, nullptr);
        context->CSSetShader(particleResetCS_.Get(), nullptr, 0);
        context->Dispatch(UINT((particleCapacity_ + 63) / 64), 1, 1);
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    ID3D11Buffer* cbuffer = particleUpdateCB_.Get();
    context->CSSetConstantBuffers(0, 1, &cbuffer);
    ID3D11UnorderedAccessView* uavs[6] = {
        particleStateUAV_.Get(), particleUAVAdditive_.Get(), particleUAVAlpha_.Get(),
        particleUAVFire_.Get(), particleCounterUAV_.Get(), particleIndirectUAV_.Get()};
    context->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
    ID3D11ShaderResourceView* spawnView = particleSpawnSRV_.Get();
    context->CSSetShaderResources(0, 1, &spawnView);

    if (!spawns.empty()) {
        context->CSSetShader(particleSpawnCS_.Get(), nullptr, 0);
        const UINT groups = UINT((spawns.size() + 63) / 64);
        context->Dispatch(groups, 1, 1);
    }
    context->CSSetShader(particleIntegrateCS_.Get(), nullptr, 0);
    const UINT integrateGroups = UINT((particleCapacity_ + 63) / 64);
    context->Dispatch(integrateGroups, 1, 1);
    context->CSSetShader(particleFinalizeCS_.Get(), nullptr, 0);
    context->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* nullUavs[6] = {nullptr, nullptr, nullptr,
                                              nullptr, nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 6, nullUavs, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->CSSetShaderResources(0, 1, &nullSrv);
    context->CSSetShader(nullptr, nullptr, 0);

    // The counts feed the UI and the first-frame log; the draws use the indirect
    // args, so they never depend on this readback. Only one copy may be in
    // flight: re-ending a pending event query would never signal.
    if (!particleReadbackPending_) {
        context->CopyResource(particleCounterStaging_.Get(), particleCounterBuffer_.Get());
        context->End(particleReadbackQuery_.Get());
        particleReadbackPending_ = true;
    }
}

void Renderer::ReadParticleCounters(sim::ParticleSystem& particles) {
    if (!particleReadbackPending_) {
        return;
    }
    ID3D11DeviceContext* context = device_->Context();
    BOOL done = FALSE;
    const HRESULT result = context->GetData(particleReadbackQuery_.Get(), &done,
                                            sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (result != S_OK || !done) {
        return;
    }
    particleReadbackPending_ = false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(particleCounterStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    uint32_t values[4] = {0, 0, 0, 0};
    memcpy(values, mapped.pData, sizeof(values));
    context->Unmap(particleCounterStaging_.Get(), 0);

    info_.additiveParticles = int(values[0]);
    info_.alphaParticles = int(values[1]);
    info_.fireParticles = int(values[2]);
    particles.SetGpuAliveCount(int(values[3]));
}

void Renderer::UpdateBloomConstants(float sourceWidth, float sourceHeight, float threshold,
                                    float intensity) {
    bloomConstants_.texelSize = XMFLOAT4(1.0f / std::max(sourceWidth, 1.0f),
                                         1.0f / std::max(sourceHeight, 1.0f), 0.0f, 0.0f);
    bloomConstants_.bloomParams.x = threshold;
    bloomConstants_.bloomParams.y = intensity;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ID3D11DeviceContext* context = device_->Context();
    if (SUCCEEDED(context->Map(bloomConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &bloomConstants_, sizeof(bloomConstants_));
        context->Unmap(bloomConstantBuffer_.Get(), 0);
    }
}

void Renderer::UpdateFrameConstants(const sim::Simulation& simulation, const Camera& camera,
                                    const RenderSettings& settings) {
    const sim::BlastModel& blast = simulation.Blast();
    const sim::BlastState& state = blast.State();
    const sim::Environment& environment = blast.GetEnvironment();

    const XMMATRIX view = camera.ViewMatrix();
    const XMMATRIX projection = camera.ProjectionMatrix();
    const XMMATRIX viewProjection = view * projection;
    const XMMATRIX inverseViewProjection = XMMatrixInverse(nullptr, viewProjection);

    XMStoreFloat4x4(&frameConstants_.viewProj, viewProjection);
    XMStoreFloat4x4(&frameConstants_.invViewProj, inverseViewProjection);

    // Billboard axes from the camera orientation (independent of the matrix
    // storage convention).
    XMFLOAT3 cameraRight{};
    XMFLOAT3 cameraUp{};
    camera.Basis(cameraRight, cameraUp);

    const XMFLOAT3 position = camera.Position();
    frameConstants_.cameraPosition =
        XMFLOAT4(position.x, position.y, position.z, float(state.time));

    const float azimuth = XMConvertToRadians(settings.sunAzimuthDeg);
    const float elevation = XMConvertToRadians(settings.sunElevationDeg);
    const XMFLOAT3 sunDirection(std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                                std::cos(elevation) * std::cos(azimuth));
    frameConstants_.sunDirection =
        XMFLOAT4(sunDirection.x, sunDirection.y, sunDirection.z, settings.exposure);
    frameConstants_.sunColor = XMFLOAT4(1.0f, 0.70f, 0.44f, settings.bloomStrength);

    if (activeVolumeValid_) {
        frameConstants_.volumeMin =
            XMFLOAT4(activeVolumeBounds_.min.x, activeVolumeBounds_.min.y,
                     activeVolumeBounds_.min.z, activeVolumeExtinction_);
        frameConstants_.volumeMax =
            XMFLOAT4(activeVolumeBounds_.max.x, activeVolumeBounds_.max.y,
                     activeVolumeBounds_.max.z, float(settings.raymarchSteps));
    } else {
        frameConstants_.volumeMin = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.1f);
        frameConstants_.volumeMax = XMFLOAT4(1.0f, 1.0f, 1.0f, float(settings.raymarchSteps));
    }

    frameConstants_.screenParams = XMFLOAT4(float(device_->Width()), float(device_->Height()),
                                            1.0f / std::max(1.0f, float(device_->Width())),
                                            1.0f / std::max(1.0f, float(device_->Height())));

    const double scorch = std::max(blast.RadiusForThermalFluence(40000.0),
                                   state.fireballRadius * 1.6);
    const double blastRadius = blast.RadiusForOverpressure(34474.0);
    frameConstants_.misc = XMFLOAT4(float(scorch), float(blastRadius),
                                    float(state.dustRingRadius),
                                    float(environment.temperatureK));

    frameConstants_.misc2 = XMFLOAT4(float(state.cloudRadius), float(state.cloudCenterY),
                                     float(state.fireballRadius),
                                     float(state.fireballTemperatureK));
    frameConstants_.misc3 = XMFLOAT4(float(state.time), float(state.time - lastSimTime_),
                                     float(frameIndex_), float(state.shockRadius));
    lastSimTime_ = state.time;

    // Burst centre and shock strength for the atmospheric refraction pass.
    frameConstants_.blast =
        XMFLOAT4(0.0f, float(blast.GetCharge().heightOfBurstM), 0.0f,
                 float(state.shockMach));

    frameConstants_.wind = XMFLOAT4(float(environment.WindX()), 0.0f,
                                    float(environment.WindZ()), float(environment.windSpeedMs));
    frameConstants_.cameraRight = XMFLOAT4(cameraRight.x, cameraRight.y, cameraRight.z, 0.0f);
    frameConstants_.cameraUp = XMFLOAT4(cameraUp.x, cameraUp.y, cameraUp.z, 0.0f);

    const float targetSpacing = std::max(camera.Distance() / 12.0f, 1.0f);
    const float exponent = std::floor(std::log10(targetSpacing));
    const float spacing = std::clamp(std::pow(10.0f, exponent), 1.0f, 5000.0f);
    frameConstants_.gridParams =
        XMFLOAT4(spacing, 1.0f, settings.showGrid ? 0.06f : 0.0f,
                 std::max(camera.Distance() * 8.0f, 500.0f));

    frameConstants_.rangeParams = XMFLOAT4(camera.NearPlane(), camera.FarPlane(),
                                           (activeVolumeValid_ && settings.enableVolume) ? 1.0f
                                                                                         : 0.0f,
                                           settings.particleScale);

    const float fireballLight =
        state.fireballVisible
            ? std::clamp(float((state.fireballTemperatureK - 700.0) / 5000.0), 0.0f, 1.4f)
            : 0.0f;
    frameConstants_.fireball =
        XMFLOAT4(0.0f, float(state.fireballCenterY), 0.0f, fireballLight);

    const bool highFidelity =
        simulation.Settings().mode == sim::SimulationMode::HighFidelity;
    if (highFidelity) {
        const sim::VolumeBounds& lightBounds = solver_.Bounds();
        frameConstants_.lightVolumeMin =
            XMFLOAT4(lightBounds.min.x, lightBounds.min.y, lightBounds.min.z, 0.0f);
        frameConstants_.lightVolumeMax =
            XMFLOAT4(lightBounds.max.x, lightBounds.max.y, lightBounds.max.z, 0.0f);
    } else {
        frameConstants_.lightVolumeMin = frameConstants_.volumeMin;
        frameConstants_.lightVolumeMax = frameConstants_.volumeMax;
    }
    const float volumeResolution = highFidelity
                                       ? float(std::max(solver_.Stats().resolution, 1))
                                       : float(std::max(volumeResolution_, 1));
    const float voxelSize =
        (activeVolumeBounds_.max.x - activeVolumeBounds_.min.x) / volumeResolution;
    frameConstants_.volumeParams = XMFLOAT4(voxelSize,
                                            activeVolumeSplit_ ? 1.0f : 0.0f,
                                            1.0f, 1.0f);
    frameConstants_.volumeParams2 =
        XMFLOAT4(highFidelity ? 0.40f : 1.0f, highFidelity ? 0.16f : 1.0f,
                 highFidelity ? 0.0f : 1.0f,
                 highFidelity ? 1.0f : 0.0f);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ID3D11DeviceContext* context = device_->Context();
    if (SUCCEEDED(context->Map(frameConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                               &mapped))) {
        memcpy(mapped.pData, &frameConstants_, sizeof(frameConstants_));
        context->Unmap(frameConstantBuffer_.Get(), 0);
    }
}

void Renderer::SetViewport(int width, int height) {
    D3D11_VIEWPORT viewport{};
    viewport.Width = float(width);
    viewport.Height = float(height);
    viewport.MaxDepth = 1.0f;
    device_->Context()->RSSetViewports(1, &viewport);
}

void Renderer::ResetBloom() {
    if (!device_) {
        return;
    }
    ID3D11DeviceContext* context = device_->Context();
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (Target& target : bloom_) {
        if (target.rtv) {
            context->ClearRenderTargetView(target.rtv.Get(), clear);
        }
    }
}

void Renderer::RenderBloom() {
    ID3D11DeviceContext* context = device_->Context();

    ID3D11ShaderResourceView* source = hdr_.srv.Get();
    int sourceWidth = hdr_.width;
    int sourceHeight = hdr_.height;

    context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    context->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);
    context->PSSetShader(downsamplePS_.Get(), nullptr, 0);

    for (int level = 0; level < kBloomLevels; ++level) {
        Target& destination = bloom_[level];
        UpdateBloomConstants(float(sourceWidth), float(sourceHeight), level == 0 ? 1.15f : 0.0f,
                             1.0f);

        ID3D11RenderTargetView* rtv = destination.rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        SetViewport(destination.width, destination.height);
        context->PSSetShaderResources(0, 1, &source);
        context->Draw(3, 0);

        source = destination.srv.Get();
        sourceWidth = destination.width;
        sourceHeight = destination.height;
    }

    context->PSSetShader(upsamplePS_.Get(), nullptr, 0);
    context->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xffffffff);

    for (int level = kBloomLevels - 1; level > 0; --level) {
        Target& destination = bloom_[level - 1];
        Target& origin = bloom_[level];
        UpdateBloomConstants(float(origin.width), float(origin.height), 0.0f, 0.85f);

        ID3D11RenderTargetView* rtv = destination.rtv.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        SetViewport(destination.width, destination.height);

        ID3D11ShaderResourceView* input = origin.srv.Get();
        context->PSSetShaderResources(0, 1, &input);
        context->Draw(3, 0);
    }

    ID3D11ShaderResourceView* nullSRV[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, nullSRV);
}

EulerConfig Renderer::MakeEulerConfig(const sim::Simulation& simulation) const {
    const sim::BlastModel& blast = simulation.Blast();
    const sim::Charge& charge = blast.GetCharge();
    const sim::Environment& environment = blast.GetEnvironment();

    EulerConfig config;
    config.resolution = std::clamp(simulation.Settings().solverResolution, 32, 512);

    const double fireball = std::max(blast.FireballMaxRadiusM(), 0.1);
    // Scale the cube with the charge: the old 30 m cap made large charges use
    // a tiny high-fidelity domain.
    config.domainHalfExtent = std::clamp(3.0 * fireball, 1.0, 2000.0);
    config.hemispherical = simulation.Settings().solverHemisphere;
    config.chargeMassKg = std::max(charge.tntMassKg, 1e-6);
    config.chargeEnergyJ = charge.EnergyJ();
    config.heightOfBurstM =
        std::clamp(charge.heightOfBurstM, 0.05, 2.0 * config.domainHalfExtent - 0.05);
    config.ambientDensity = environment.AirDensity();
    config.ambientPressure = environment.pressurePa;
    config.ambientTemperature = environment.VirtualTemperatureK();
    config.gamma = environment.gamma;
    config.gasConstant = environment.gasConstant;
    config.gravity = environment.gravity;
    config.fireballRadiusM = fireball;
    return config;
}

void Renderer::MaybeExpandSolver(const sim::Simulation& simulation) {
    if (!simulation.Settings().solverAutoScale || !solver_.HasVolume()) {
        return;
    }

    constexpr double kTriggerFraction = 0.78;
    constexpr double kFieldMargin = 1.12;
    constexpr float kExpansion = 1.3f;
    constexpr float kMaxAccuracy = 16.0f;

    // Follow the measured plume: the analytic cloud overestimates the early
    // cap, and expanding to fit it coarsens the grid before the vortex ring has
    // formed. The analytic model is only a per-component fallback until the
    // reduction has measured a radius/top.
    const sim::BlastState& state = simulation.Blast().State();
    const double analyticHalf = std::max({state.fireballRadius * 2.4,
                                          state.cloudRadius * 1.35, 0.5});
    const double analyticTop =
        std::max({state.fireballCenterY + state.fireballRadius * 2.0,
                  state.cloudCenterY + state.cloudRadius * 1.6, 1.0});

    double requiredHalf = analyticHalf;
    double requiredTop = analyticTop;
    double measuredDome = 0.0;
    if (solver_.HasFieldBounds()) {
        const double measuredRadius = double(solver_.FieldRadius());
        const double measuredTop = double(solver_.FieldTop());
        if (measuredRadius > 0.0) {
            requiredHalf = std::max(measuredRadius * kFieldMargin, 0.5);
        }
        if (measuredTop > 0.0) {
            requiredTop = std::max(measuredTop * kFieldMargin, 1.0);
        }
        measuredDome = double(solver_.FieldDomeFraction());
    }
    const bool hemisphere = simulation.Settings().solverHemisphere;

    // A single 1.3x step is not always enough after a fast plume rise, so keep
    // expanding until the field fits with margin (or the accuracy budget is
    // exhausted).
    for (int attempt = 0; attempt < 4; ++attempt) {
        const sim::VolumeBounds& bounds = solver_.Bounds();
        const double half = 0.5 * (bounds.max.x - bounds.min.x);
        const double top = bounds.max.y;
        const bool tooWide = requiredHalf > half * kTriggerFraction;
        const bool tooTall = requiredTop > top * kTriggerFraction;
        const bool tooDome = hemisphere && measuredDome > kTriggerFraction;
        if (!tooWide && !tooTall && !tooDome) {
            break;
        }
        if (solver_.Stats().accuracyFactor >= kMaxAccuracy) {
            if (!solverCapWarned_) {
                LogWarn("High-fidelity auto-scale reached the accuracy budget (x%.1f); "
                        "the plume may leave the domain.",
                        solver_.Stats().accuracyFactor);
                solverCapWarned_ = true;
            }
            break;
        }
        if (!solver_.RescaleDomain(kExpansion)) {
            break;
        }
    }
}

void Renderer::UpdatePlumeModel(sim::Simulation& simulation) {
    const sim::BlastState& state = simulation.Blast().State();
    // The analytic model is the reference cloud: it drives the camera, the dust
    // and the particles, and it is what the UI reports. Use its cap ellipsoid
    // (major 0.62 R, minor 0.52 R) and its rise speed so the resolved head
    // follows the same geometry and trajectory instead of drifting below it.
    const float cloudRadius = float(state.cloudRadius);
    const float radius = 0.62f * cloudRadius;
    const float centerY = float(state.cloudCenterY);
    const float capTop = centerY + 0.52f * cloudRadius;
    solver_.SetPlumeModel(radius, centerY, capTop, cloudRadius,
                          float(state.cloudRiseSpeed));
}

bool Renderer::EnsureSolver(sim::Simulation& simulation) {
    if (solverBlastSerial_ != simulation.BlastSerial()) {
        if (solver_.Reset(MakeEulerConfig(simulation))) {
            solverBlastSerial_ = simulation.BlastSerial();
            solverCapWarned_ = false;
            solverPrepared_ = false;
            solverBudgetHit_ = false;
        } else {
            LogWarn("High-fidelity solver reset failed.");
            return false;
        }
    }
    return solver_.HasVolume();
}

double Renderer::AdvanceSolver(sim::Simulation& simulation, float realDt) {
    if (solverPaused_ || simulation.IsArmed()) {
        solverBudgetHit_ = false;
        return 0.0;
    }
    if (!EnsureSolver(simulation)) {
        solverBudgetHit_ = false;
        return 0.0;
    }

    MaybeExpandSolver(simulation);

    // The playback time scale is the target: at 1.0 the solver advances one
    // simulated second per real second. The solver's CFL step is smaller, so
    // submit enough steps to cover the frame, but never more than the
    // per-frame budget. The budget keeps the frame time balanced across charge
    // sizes; when it is reached the playback simply runs slower.
    const double target =
        std::max(0.0, double(realDt) * simulation.Settings().timeScale);
    if (target <= 0.0) {
        solverBudgetHit_ = false;
        return 0.0;
    }

    const int maxSteps =
        std::clamp(simulation.Settings().solverStepsPerFrame, 8, 256);
    const int batchMax = std::min(maxSteps, 64);
    double advancedTotal = 0.0;
    int submitted = 0;
    while (advancedTotal + 1e-9 < target && submitted < maxSteps) {
        const float dt = solver_.Stats().dt;
        int steps = 1;
        if (dt > 0.0f) {
            steps = int(std::ceil((target - advancedTotal) / double(dt)));
            steps = std::clamp(steps, 1, std::min(batchMax, maxSteps - submitted));
        }
        UpdatePlumeModel(simulation);
        solver_.Step(steps);
        const double advanced = solver_.ConsumeAdvance();
        advancedTotal += advanced;
        submitted += steps;
        if (advanced <= 0.0) {
            break;
        }
    }
    solverBudgetHit_ = advancedTotal + 1e-9 < target;
    solverPrepared_ = true;
    if (submitted > 0) {
        // One render-volume update per frame, after all solver steps. Step() no
        // longer does this, so several Step() calls share a single pass.
        solver_.UpdateRenderVolume();
    }
    return advancedTotal;
}

void Renderer::PrepareHighFidelity(sim::Simulation& simulation, double targetTime) {
    if (!solver_.Reset(MakeEulerConfig(simulation))) {
        LogError("High-fidelity solver could not be initialised.");
        solverPrepared_ = false;
        return;
    }
    solverBlastSerial_ = simulation.BlastSerial();
    solverCapWarned_ = false;
    solver_.SetDeterministic(true);

    const auto start = std::chrono::steady_clock::now();
    int guard = 0;
    while (solver_.Stats().time < targetTime && guard++ < 4096) {
        MaybeExpandSolver(simulation);
        UpdatePlumeModel(simulation);
        solver_.Step(16);

        // Keep the analytic model in sync so the ground effects, dust and the
        // camera follow the solver's simulated time (the step-count path does
        // the same).
        const double advance = solver_.ConsumeAdvance();
        if (advance > 0.0) {
            simulation.AdvanceExternal(advance);
        }

        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        if (elapsed > 20.0) {
            LogWarn("High-fidelity solver stopped at %.4f s (time budget reached).",
                    solver_.Stats().time);
            break;
        }
    }
    solverPrepared_ = true;
    solver_.UpdateRenderVolume();

    LogInfo("High-fidelity: t=%.4f s (analytic %.4f s), %lld steps, %d^3, dt=%.2f us, "
            "maxSpeed=%.0f m/s",
            solver_.Stats().time, simulation.Blast().State().time, solver_.Stats().steps,
            solver_.Stats().resolution, solver_.Stats().dt * 1e6f, solver_.Stats().maxSpeed);
}

void Renderer::PrepareHighFidelitySteps(sim::Simulation& simulation, long long targetSteps) {
    if (!solver_.Reset(MakeEulerConfig(simulation))) {
        LogError("High-fidelity solver could not be initialised.");
        solverPrepared_ = false;
        return;
    }
    solverBlastSerial_ = simulation.BlastSerial();
    solverCapWarned_ = false;
    solver_.SetDeterministic(true);

    const auto start = std::chrono::steady_clock::now();
    while (solver_.Stats().steps < targetSteps) {
        MaybeExpandSolver(simulation);

        const long long remaining = targetSteps - solver_.Stats().steps;
        const int batch = int(std::min<long long>(remaining, 16));
        UpdatePlumeModel(simulation);
        solver_.Step(batch);

        const double advance = solver_.ConsumeAdvance();
        if (advance > 0.0) {
            simulation.AdvanceExternal(advance);
        }

        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        if (elapsed > 180.0) {
            LogWarn("High-fidelity capture stopped at step %lld (time budget reached).",
                    solver_.Stats().steps);
            break;
        }
    }
    solverPrepared_ = true;
    solver_.UpdateRenderVolume();

    LogInfo("High-fidelity capture: %lld steps, t=%.4f s (analytic %.4f s), domain half %.2f m, "
            "top %.2f m, accuracy x%.2f, expansions %d, plume top %.2f m, radius %.2f m, "
            "cap radius %.2f m, analytic cloud R %.2f m, centre %.2f m, rise %.2f m/s",
            solver_.Stats().steps, solver_.Stats().time, simulation.Blast().State().time,
            solver_.Stats().domainHalfExtent, solver_.Stats().domainTop,
            solver_.Stats().accuracyFactor, solver_.Stats().expansions,
            solver_.Stats().plumeTop, solver_.Stats().plumeRadius,
            solver_.Stats().plumeCapRadius,
            simulation.Blast().State().cloudRadius,
            simulation.Blast().State().cloudCenterY,
            simulation.Blast().State().cloudRiseSpeed);
}

double Renderer::ConsumeSolverAdvance() {    return solver_.ConsumeAdvance();
}

void Renderer::StepSolverOnce(int steps) {
    solver_.Step(steps);
    solver_.UpdateRenderVolume();
}

SolverBenchmarkResult Renderer::BenchmarkSolver(sim::Simulation& simulation,
                                                long long steps, int stepsPerFrame) {
    SolverBenchmarkResult result;
    if (steps <= 0) {
        return result;
    }
    if (!solver_.Reset(MakeEulerConfig(simulation))) {
        LogError("Solver benchmark: solver could not be initialised.");
        return result;
    }
    solverBlastSerial_ = simulation.BlastSerial();
    solverCapWarned_ = false;
    solverPrepared_ = false;
    solverBudgetHit_ = false;

    // Non-blocking reductions: this is the interactive pipeline. The grid is
    // fixed (no auto-scale) so the resolutions are directly comparable.
    solver_.SetDeterministic(false);

    stepsPerFrame = std::clamp(stepsPerFrame, 1, 256);
    const int frames = int((steps + stepsPerFrame - 1) / stepsPerFrame);

    // Warm-up: lets the GPU clocks ramp and the reduction pipeline settle.
    const long long warmup = std::min<long long>(steps, std::max(8, stepsPerFrame * 2));
    for (long long done = 0; done < warmup;) {
        const int batch = int(std::min<long long>(stepsPerFrame, warmup - done));
        UpdatePlumeModel(simulation);
        solver_.Step(batch);
        solver_.UpdateRenderVolume();
        solver_.WaitForReduction();
        const double advance = solver_.ConsumeAdvance();
        if (advance > 0.0) {
            simulation.AdvanceExternal(advance);
        }
        done += batch;
    }
    device_->Context()->Flush();

    // 3 sweeps + at most one reduction per step, one render pass per frame.
    // The margin also covers a future separate source pass.
    if (!solver_.BeginTiming(int(steps * 5 + frames * 3 + 16))) {
        LogWarn("Solver benchmark: GPU timestamp queries are unavailable.");
    }

    const auto start = std::chrono::steady_clock::now();
    double submissionMs = 0.0;
    long long done = 0;
    for (int frame = 0; frame < frames; ++frame) {
        // Mirror Renderer::AdvanceSolver: up to 64 steps per Step() call and
        // several calls per frame when the step budget is larger.
        int remaining = int(std::min<long long>(stepsPerFrame, steps - done));
        while (remaining > 0) {
            const int batch = std::min(remaining, 64);
            UpdatePlumeModel(simulation);
            const auto submitStart = std::chrono::steady_clock::now();
            solver_.Step(batch);
            submissionMs += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - submitStart)
                                .count();
            remaining -= batch;
            const double advance = solver_.ConsumeAdvance();
            if (advance > 0.0) {
                simulation.AdvanceExternal(advance);
            }
        }
        // Once per frame, and consume the reduction at the frame boundary.
        const auto renderStart = std::chrono::steady_clock::now();
        solver_.UpdateRenderVolume();
        submissionMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - renderStart)
                            .count();
        solver_.WaitForReduction();
        done += std::min<long long>(stepsPerFrame, steps - done);
    }
    result.timing = solver_.ResolveTiming();
    result.wallMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    result.submissionMs = submissionMs;

    result.valid = result.timing.valid;
    result.resolution = solver_.Stats().resolution;
    result.steps = steps;
    result.stepsPerFrame = stepsPerFrame;
    result.frames = frames;
    result.domainHalfExtent = solver_.Stats().domainHalfExtent;
    result.voxelSize = solver_.Stats().voxelSize;
    if (result.timing.valid) {
        result.msPerStep = result.timing.totalMs / double(steps);
        result.msPerFrame = result.timing.totalMs / double(frames);
    }
    solverPrepared_ = true;
    return result;
}

void Renderer::Render(sim::Simulation& simulation, const Camera& camera,
                      const RenderSettings& settings) {
    if (!device_) {
        return;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    ID3D11DeviceContext* context = device_->Context();

    if (!EnsureTargets(device_->Width(), device_->Height())) {
        return;
    }

    // --- Volume source -----------------------------------------------------
    const bool highFidelity =
        simulation.Settings().mode == sim::SimulationMode::HighFidelity;

    const sim::VolumeFrame& volumeFrame = simulation.Volume().Current();
    ID3D11ShaderResourceView* solverVolume = nullptr;

    if (highFidelity) {
        // AdvanceSolver() has already stepped the solver for this frame. Here we
        // only bind its volume; an armed/rearmed charge shows no solver volume.
        if (!simulation.IsArmed() && EnsureSolver(simulation)) {
            solverVolume = solver_.VolumeSRV();
        }

        activeVolumeValid_ = solverVolume != nullptr;
        activeVolumeBounds_ = solver_.Bounds();
        // Wind drift: the solver runs in its own (still-air) frame; shifting the
        // render box moves the fireball and cloud downwind.
        const sim::Environment& environment = simulation.Blast().GetEnvironment();
        const double drift = solver_.Stats().time;
        const float windX = float(environment.WindX() * drift);
        const float windZ = float(environment.WindZ() * drift);
        activeVolumeBounds_.min.x += windX;
        activeVolumeBounds_.max.x += windX;
        activeVolumeBounds_.min.z += windZ;
        activeVolumeBounds_.max.z += windZ;
        activeVolumeExtinction_ = solver_.ExtinctionScale();
        activeVolumeSplit_ = false;
    } else {
        const sim::SimulationSettings& simSettings = simulation.Settings();
        bool gpuBake = simSettings.bakeVolume && simSettings.gpuVolumeBake &&
                       volumeBakeCS_ != nullptr;

        if (gpuBake) {
            sim::VolumeBakeRequest request;
            if (simulation.Volume().TakeGpuRequest(request)) {
                if (!DispatchGpuVolumeBake(request)) {
                    // Fall back to the CPU baker; it takes over on the next
                    // scheduled bake.
                    simulation.Volume().SetGpuBake(false);
                    gpuBake = false;
                    gpuVolumeBakeActive_ = false;
                }
            }
        }

        if (gpuBake) {
            activeVolumeValid_ = volumeValid_;
            activeVolumeBounds_ = gpuVolumeBounds_;
            activeVolumeExtinction_ = gpuVolumeExtinction_;
            activeVolumeSplit_ = false;
        } else {
            // CPU volume bake upload (only when a new bake is available).
            if (simulation.Volume().HasNewFrame() && !volumeFrame.empty()) {
                UploadVolume(volumeFrame);
                simulation.Volume().MarkUploaded(volumeFrame.version);
            }
            if (!simulation.Volume().HasData()) {
                volumeValid_ = false;
            }

            activeVolumeValid_ = volumeValid_ && simulation.Volume().HasData();
            activeVolumeBounds_ = volumeFrame.bounds;
            activeVolumeExtinction_ = volumeFrame.extinctionScale;
            activeVolumeSplit_ = volumeSplit_;
        }
    }

    // The active volume feeds the raymarch and the smoke shadows on the ground
    // and scene.
    ID3D11ShaderResourceView* volumeView =
        highFidelity ? solverVolume : volumeSRV_.Get();

    const int capacity = simulation.Particles().Capacity();
    bool gpuParticles = simulation.Particles().GpuMode() && SupportsGpuParticles();
    if (capacity != particleCapacity_ || gpuParticles != particleGpuMode_) {
        particleCapacity_ = capacity;
        particleGpuMode_ = gpuParticles;
        bool ok = false;
        if (gpuParticles) {
            ok = CreateParticleGpuResources(capacity);
        }
        if (!ok) {
            // Fall back to the CPU path. The particle system switches back so
            // its update loop integrates and builds the batches again.
            gpuParticles = false;
            particleGpuMode_ = false;
            simulation.Particles().SetGpuMode(false);
            if (!CreateStructuredBuffer(device_->Device(), capacity, particleBufferAdditive_,
                                        particleSRVAdditive_) ||
                !CreateStructuredBuffer(device_->Device(), capacity, particleBufferFire_,
                                        particleSRVFire_) ||
                !CreateStructuredBuffer(device_->Device(), capacity, particleBufferAlpha_,
                                        particleSRVAlpha_)) {
                LogError("Failed to create particle instance buffers.");
                return;
            }
        }
    }

    if (gpuParticles) {
        UpdateGpuParticles(simulation);
    } else {
        if (!highFidelity) {
            UploadParticles(simulation.Particles().AdditiveBatch(), particleBufferAdditive_);
            UploadParticles(simulation.Particles().AlphaBatch(), particleBufferAlpha_);
        }
        // Scene fires are rendered in both modes; in high-fidelity mode the
        // batch contains only the fires on burning scene objects.
        UploadParticles(simulation.Particles().FireBatch(), particleBufferFire_);
    }

    if (frameIndex_ == 0) {
        const auto& state = simulation.Blast().State();
        const int additive = gpuParticles ? info_.additiveParticles
                                          : int(simulation.Particles().AdditiveBatch().size());
        const int alpha = gpuParticles ? info_.alphaParticles
                                       : int(simulation.Particles().AlphaBatch().size());
        const int fire = gpuParticles ? info_.fireParticles
                                      : int(simulation.Particles().FireBatch().size());
        LogInfo("First frame: t=%.3fs fireball=%.2fm at y=%.2f (%.0f K) cloud y=%.2f "
                "shock=%.1fm volume=%d particles=%d+%d fire=%d highFidelity=%d",
                state.time, state.fireballRadius, state.fireballCenterY,
                state.fireballTemperatureK, state.cloudCenterY, state.shockRadius,
                activeVolumeValid_ ? 1 : 0, additive, alpha, fire, highFidelity ? 1 : 0);
    }

    UpdateFrameConstants(simulation, camera, settings);

    ID3D11Buffer* constants[2] = {frameConstantBuffer_.Get(), bloomConstantBuffer_.Get()};
    context->VSSetConstantBuffers(0, 2, constants);
    context->PSSetConstantBuffers(0, 2, constants);

    context->PSSetSamplers(0, 1, linearClamp_.GetAddressOf());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->RSSetState(rasterizer_.Get());

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(hdr_.rtv.Get(), clearColor);
    context->ClearDepthStencilView(device_->DepthDSV(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // --- Sky ---------------------------------------------------------------
    ID3D11RenderTargetView* hdrRTV = hdr_.rtv.Get();
    context->OMSetRenderTargets(1, &hdrRTV, nullptr);
    context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    context->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);
    SetViewport(hdr_.width, hdr_.height);
    context->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
    context->PSSetShader(skyPS_.Get(), nullptr, 0);
    context->Draw(3, 0);

    // --- Cascaded directional light matrices -------------------------------
    // Computed unconditionally so the ground always has valid light matrices,
    // even when the procedural scene is disabled.
    {
        const float sceneExtent =
            simulation.Settings().scene.enabled ? simulation.Scene().Extent() : 0.0f;
        const float extent = std::max(sceneExtent, 60.0f);
        const XMVECTOR sunDirection =
            XMVector3Normalize(XMVectorSet(frameConstants_.sunDirection.x,
                                           frameConstants_.sunDirection.y,
                                           frameConstants_.sunDirection.z, 0.0f));
        const float nearPlane = std::max(camera.NearPlane(), 0.2f);
        const float farPlane =
            std::max({extent * 2.0f, 320.0f, camera.Distance() * 3.0f});
        constexpr float kSplitLambda = 0.7f;
        float splits[4] = {nearPlane, 0.0f, 0.0f, 0.0f};
        for (int i = 1; i <= 3; ++i) {
            const float p = float(i) / 3.0f;
            const float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
            const float uniform = nearPlane + (farPlane - nearPlane) * p;
            splits[i] = kSplitLambda * logSplit + (1.0f - kSplitLambda) * uniform;
        }

        const XMMATRIX inverseView = XMMatrixInverse(nullptr, camera.ViewMatrix());
        const float aspect =
            float(hdr_.width) / float(std::max(hdr_.height, 1));
        const float tanY = std::tan(camera.FovY() * 0.5f);
        const float tanX = tanY * aspect;
        const XMVECTOR up = std::abs(XMVectorGetY(sunDirection)) > 0.95f
                                ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                                : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        for (int cascade = 0; cascade < 3; ++cascade) {
            // Overlap the next cascade backwards by the blend band so both
            // maps cover the last 10 % of the previous cascade.
            constexpr float kBlendWidth = 0.1f;
            float nearZ = splits[cascade];
            if (cascade > 0) {
                nearZ = std::max(
                    splits[cascade - 1],
                    splits[cascade] - kBlendWidth *
                                          (splits[cascade] - splits[cascade - 1]));
            }
            XMVECTOR corners[8];
            int corner = 0;
            for (int side = 0; side < 2; ++side) {
                const float z = side == 0 ? nearZ : splits[cascade + 1];
                for (int sy = -1; sy <= 1; sy += 2) {
                    for (int sx = -1; sx <= 1; sx += 2) {
                        corners[corner++] = XMVector3TransformCoord(
                            XMVectorSet(sx * tanX * z, sy * tanY * z, z, 1.0f),
                            inverseView);
                    }
                }
            }
            XMVECTOR center = XMVectorZero();
            for (int i = 0; i < 8; ++i) {
                center += corners[i];
            }
            center /= 8.0f;
            float radius = 2.0f;
            for (int i = 0; i < 8; ++i) {
                radius = std::max(
                    radius, XMVectorGetX(XMVector3Length(corners[i] - center)));
            }
            const XMVECTOR eye = center + sunDirection * (radius + 2.0f);
            const XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);
            const XMMATRIX lightProjection = XMMatrixOrthographicLH(
                radius * 2.1f, radius * 2.1f, 0.1f, radius * 2.0f + 4.0f);
            XMStoreFloat4x4(&shadowConstants_.lightViewProj[cascade],
                            lightView * lightProjection);
            (&shadowConstants_.cascadeSplits.x)[cascade] = splits[cascade + 1];
            (&shadowConstants_.cascadeTexels.x)[cascade] =
                1.0f / float(SceneRenderer::ShadowSize(cascade));
            (&shadowConstants_.cascadeWorldTexels.x)[cascade] =
                radius * 2.1f / float(SceneRenderer::ShadowSize(cascade));
        }
        shadowConstants_.cascadeSplits.w = farPlane;

        // Static world-space map for the intact buildings. It covers the town
        // and the longest shadows of its tallest buildings, and is independent
        // of the camera, so it can be cached across frames.
        const float sunY = std::max(XMVectorGetY(sunDirection), 0.2f);
        const float shadowLength =
            30.0f * std::sqrt(std::max(1.0f - sunY * sunY, 0.0f)) / sunY;
        const float staticRadius =
            std::max(extent * 1.2f, 60.0f) + 20.0f + shadowLength * 0.5f;
        const XMVECTOR staticCenter = XMVectorSet(0.0f, 15.0f, 0.0f, 1.0f);
        const XMVECTOR staticEye =
            staticCenter + sunDirection * (staticRadius * 2.0f);
        const XMMATRIX staticView =
            XMMatrixLookAtLH(staticEye, staticCenter, up);
        const XMMATRIX staticProjection = XMMatrixOrthographicLH(
            staticRadius * 2.0f, staticRadius * 2.0f, 0.1f,
            staticRadius * 4.0f);
        XMStoreFloat4x4(&shadowConstants_.staticLightViewProj,
                        staticView * staticProjection);
        shadowConstants_.staticTexel = XMFLOAT4(
            1.0f / float(SceneRenderer::kStaticShadowSize),
            staticRadius * 2.0f / float(SceneRenderer::kStaticShadowSize),
            0.05f / (staticRadius * 4.0f), 0.0f);

        D3D11_MAPPED_SUBRESOURCE shadowMapped{};
        if (SUCCEEDED(context->Map(shadowConstantBuffer_.Get(), 0,
                                   D3D11_MAP_WRITE_DISCARD, 0, &shadowMapped))) {
            std::memcpy(shadowMapped.pData, &shadowConstants_,
                        sizeof(shadowConstants_));
            context->Unmap(shadowConstantBuffer_.Get(), 0);
        }
    }
    ID3D11Buffer* shadowBuffer = shadowConstantBuffer_.Get();
    context->VSSetConstantBuffers(2, 1, &shadowBuffer);
    context->PSSetConstantBuffers(2, 1, &shadowBuffer);

    // --- Shadow map ---------------------------------------------------------
    SceneRenderer::ViewInfo sceneView{};
    XMStoreFloat4x4(&sceneView.viewProj, camera.ViewProjectionMatrix());
    sceneView.eye = camera.Position();
    sceneView.projScale =
        float(hdr_.height) / (2.0f * std::tan(camera.FovY() * 0.5f));
    // The static cache only depends on the scene, the sun and the set of
    // collapsed buildings, not on the camera.
    uint64_t staticShadowKey = simulation.Scene().GenerationHash();
    staticShadowKey ^=
        uint64_t(int(std::lround(settings.sunAzimuthDeg * 10.0f))) *
        0x9E3779B97F4A7C15ull;
    staticShadowKey ^=
        uint64_t(int(std::lround(settings.sunElevationDeg * 10.0f))) << 32;
    int collapsedBuildings = 0;
    for (const scene::SceneObject& object : simulation.Scene().Objects()) {
        if (object.collapsed) {
            ++collapsedBuildings;
        }
    }
    staticShadowKey ^= uint64_t(collapsedBuildings) * 0xBF58476D1CE4E5B9ull;

    sceneRenderer_.RenderStaticShadow(context, simulation.Scene(),
                                      shadowConstants_.staticLightViewProj,
                                      staticShadowKey);
    const XMFLOAT3 burstCenter(
        0.0f, float(simulation.Blast().GetCharge().heightOfBurstM), 0.0f);
    sceneRenderer_.RenderShadow(context, simulation.Scene(), sceneView,
                                shadowConstants_.lightViewProj, burstCenter);

    // --- Ground ------------------------------------------------------------
    if (settings.enableGround) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthWrite_.Get(), 0);
        context->IASetInputLayout(nullptr);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
        context->PSSetShader(groundPS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* groundShadow[4] = {
            sceneRenderer_.ShadowSRV(0), sceneRenderer_.ShadowSRV(1),
            sceneRenderer_.ShadowSRV(2), sceneRenderer_.StaticShadowSRV()};
        context->PSSetShaderResources(2, 4, groundShadow);
        ID3D11SamplerState* groundShadowSampler = sceneRenderer_.ShadowSampler();
        context->PSSetSamplers(2, 1, &groundShadowSampler);

        // Smoke shadow lookup for the ground.
        ID3D11ShaderResourceView* smokeVolume =
            volumeView ? volumeView : volumeDummySRV_.Get();
        context->PSSetShaderResources(6, 1, &smokeVolume);
        ID3D11SamplerState* smokeSampler = linearClamp_.Get();
        context->PSSetSamplers(3, 1, &smokeSampler);

        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullShadow[4] = {nullptr, nullptr, nullptr,
                                                   nullptr};
        context->PSSetShaderResources(2, 4, nullShadow);
        ID3D11ShaderResourceView* nullSmoke = nullptr;
        context->PSSetShaderResources(6, 1, &nullSmoke);
        ID3D11SamplerState* nullSampler = nullptr;
        context->PSSetSamplers(2, 1, &nullSampler);
        context->PSSetSamplers(3, 1, &nullSampler);
        context->PSSetSamplers(0, 1, linearClamp_.GetAddressOf());
    }

    // --- Procedural scene ---------------------------------------------------
    if (simulation.Settings().scene.enabled && !simulation.Scene().Empty()) {
        ID3D11SamplerState* surfaceSampler = linearWrap_.Get();
        if (settings.anisotropicFiltering >= 8) {
            surfaceSampler = anisoWrap_[2].Get();
        } else if (settings.anisotropicFiltering >= 4) {
            surfaceSampler = anisoWrap_[1].Get();
        } else if (settings.anisotropicFiltering >= 2) {
            surfaceSampler = anisoWrap_[0].Get();
        }
        ID3D11ShaderResourceView* smokeVolume =
            volumeView ? volumeView : volumeDummySRV_.Get();
        context->PSSetShaderResources(7, 1, &smokeVolume);
        context->PSSetSamplers(4, 1, linearClamp_.GetAddressOf());
        sceneRenderer_.RenderScene(context, simulation.Scene(), hdrRTV,
                                   device_->DepthDSV(), hdr_.width, hdr_.height,
                                   surfaceSampler);
        ID3D11ShaderResourceView* nullSmoke = nullptr;
        context->PSSetShaderResources(7, 1, &nullSmoke);
        ID3D11SamplerState* nullSmokeSampler = nullptr;
        context->PSSetSamplers(4, 1, &nullSmokeSampler);
    }

    // Snapshot the depth buffer so subsequent passes can sample it without a
    // read/write hazard on the depth-stencil view.
    context->OMSetRenderTargets(1, &hdrRTV, nullptr);
    context->CopyResource(depthCopyTexture_.Get(), device_->DepthTexture());
    ID3D11ShaderResourceView* depthSample = depthCopySRV_.Get();

    if (!gpuParticles) {
        info_.additiveParticles =
            highFidelity ? 0 : int(simulation.Particles().AdditiveBatch().size());
        info_.fireParticles = int(simulation.Particles().FireBatch().size());
        info_.alphaParticles =
            highFidelity ? 0 : int(simulation.Particles().AlphaBatch().size());
    }
    info_.raymarchSteps = settings.raymarchSteps;
    info_.volumeValid = activeVolumeValid_;
    info_.highFidelity = highFidelity;

    // Hot debris and sparks are drawn before the volume so that the optically
    // thick fireball correctly occludes the material inside it.
    if (!highFidelity && settings.enableParticles && (gpuParticles || info_.additiveParticles > 0)) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthTest_.Get(), 0);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(particleVS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {particleSRVAdditive_.Get(), depthSample};
        context->VSSetShaderResources(0, 1, srvs);
        context->PSSetShaderResources(0, 2, srvs);
        context->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(particleAdditivePS_.Get(), nullptr, 0);
        if (gpuParticles) {
            context->DrawInstancedIndirect(particleIndirectBuffer_.Get(), 0);
        } else {
            context->DrawInstanced(6, UINT(info_.additiveParticles), 0, 0);
        }

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->VSSetShaderResources(0, 1, nullSRVs);
        context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // --- Fire particles -----------------------------------------------------
    // Drawn before the volume so the smoke's premultiplied transmittance
    // composites over the flames: the cloud occludes them and scatters its own
    // light over the fire.
    if (settings.enableParticles && (gpuParticles || info_.fireParticles > 0)) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthTest_.Get(), 0);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(particleVS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {particleSRVFire_.Get(), depthSample};
        context->VSSetShaderResources(0, 1, srvs);
        context->PSSetShaderResources(0, 2, srvs);
        context->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(particleFirePS_.Get(), nullptr, 0);
        if (gpuParticles) {
            context->DrawInstancedIndirect(particleIndirectBuffer_.Get(), 32);
        } else {
            context->DrawInstanced(6, UINT(info_.fireParticles), 0, 0);
        }

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->VSSetShaderResources(0, 1, nullSRVs);
        context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // --- Volumetric fireball / smoke ---------------------------------------
    if (settings.enableVolume && activeVolumeValid_ && volumeView) {
        // Refresh the sun transmittance volume for the HF solver. The light
        // volume is view-independent and sampled by the raymarcher.
        if (highFidelity && solverVolume) {
            const XMFLOAT4& sun = frameConstants_.sunDirection;
            solver_.UpdateLightVolume(XMFLOAT3(sun.x, sun.y, sun.z));
        }

        context->OMSetRenderTargets(1, &hdrRTV, nullptr);
        context->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* lightView = volumeDummySRV_.Get();
        if (highFidelity && solverVolume) {
            if (ID3D11ShaderResourceView* light = solver_.LightVolumeSRV()) {
                lightView = light;
            }
        }
        ID3D11ShaderResourceView* srvs[5] = {volumeView, depthSample, noiseSRV_.Get(),
                                             volumeAuxSRV_ ? volumeAuxSRV_.Get()
                                                           : volumeDummySRV_.Get(),
                                             lightView};
        context->PSSetShaderResources(0, 5, srvs);
        ID3D11SamplerState* volumeSampler = linearClamp_.Get();
        if (settings.anisotropicFiltering >= 8) {
            volumeSampler = anisoClamp_[2].Get();
        } else if (settings.anisotropicFiltering >= 4) {
            volumeSampler = anisoClamp_[1].Get();
        } else if (settings.anisotropicFiltering >= 2) {
            volumeSampler = anisoClamp_[0].Get();
        }
        ID3D11SamplerState* samplers[2] = {volumeSampler, noiseSampler_.Get()};
        context->PSSetSamplers(0, 2, samplers);
        context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
        context->OMSetBlendState(blendPremultiplied_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(volumePS_.Get(), nullptr, 0);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRVs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
        context->PSSetShaderResources(0, 5, nullSRVs);
        context->PSSetSamplers(0, 1, linearClamp_.GetAddressOf());
    }

    // Soft smoke and dust puffs composite over the volume.
    if (!highFidelity && settings.enableParticles && (gpuParticles || info_.alphaParticles > 0)) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthTest_.Get(), 0);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(particleVS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {particleSRVAlpha_.Get(), depthSample};
        context->VSSetShaderResources(0, 1, srvs);
        context->PSSetShaderResources(0, 2, srvs);
        context->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(particleAlphaPS_.Get(), nullptr, 0);
        if (gpuParticles) {
            context->DrawInstancedIndirect(particleIndirectBuffer_.Get(), 16);
        } else {
            context->DrawInstanced(6, UINT(info_.alphaParticles), 0, 0);
        }

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->VSSetShaderResources(0, 1, nullSRVs);
        context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // --- Shock-front refraction ---------------------------------------------
    // The air compressed by the blast front has a higher refractive index
    // (Gladstone-Dale: n - 1 = K rho), so the front acts as a thin spherical
    // lens. The pass bends the already rendered scene along the projected
    // density gradient. It runs before bloom so the distorted image is what
    // gets bloomed.
    const sim::BlastState& shockState = simulation.Blast().State();
    if (shockState.time > 1e-4 && shockState.shockRadius > 0.5 &&
        shockState.shockMach > 1.01 && hdrCopy_.texture) {
        // The source is still bound as a render target; unbind it before the
        // copy so the runtime does not see a read/write conflict.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(hdrCopy_.texture.Get(), hdr_.texture.Get());
        context->OMSetRenderTargets(1, &hdrRTV, nullptr);
        context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
        context->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);
        SetViewport(hdr_.width, hdr_.height);
        ID3D11ShaderResourceView* shockInputs[2] = {hdrCopy_.srv.Get(), depthSample};
        context->PSSetShaderResources(0, 2, shockInputs);
        context->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
        context->PSSetShader(shockPS_.Get(), nullptr, 0);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* nullShock[2] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, nullShock);
    }

    // --- Post processing ---------------------------------------------------
    if (settings.enableBloom) {
        RenderBloom();
    }

    device_->BindFinalTarget();
    context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    context->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);

    bloomConstants_.bloomParams.z = settings.exposure;
    // Do not leak a stale bloom texture into the composite when bloom is off.
    bloomConstants_.bloomParams.w = settings.enableBloom ? settings.bloomStrength : 0.0f;
    bloomConstants_.texelSize = XMFLOAT4(1.0f / float(hdr_.width), 1.0f / float(hdr_.height),
                                         0.0f, 0.0f);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(bloomConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &bloomConstants_, sizeof(bloomConstants_));
        context->Unmap(bloomConstantBuffer_.Get(), 0);
    }

    ID3D11ShaderResourceView* compositeInputs[2] = {hdr_.srv.Get(), bloom_[0].srv.Get()};
    context->PSSetShaderResources(0, 2, compositeInputs);
    context->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
    context->PSSetShader(compositePS_.Get(), nullptr, 0);
    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, nullSRVs);


    ++frameIndex_;

    const auto end = std::chrono::high_resolution_clock::now();
    info_.cpuRenderMs = std::chrono::duration<float, std::milli>(end - start).count();
}

} // namespace ex::gfx
