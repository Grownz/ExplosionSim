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

    if (!CreateStates() || !CreatePipeline() || !CreateNoiseTexture() ||
        !EnsureTargets(device_->Width(), device_->Height())) {
        return false;
    }

    if (!solver_.Initialize(device_->Device(), device_->Context(),
                            device_->DedicatedVideoMemory())) {
        LogWarn("High-fidelity GPU solver unavailable.");
    }
    return true;
}

void Renderer::Shutdown() {
    solver_.Shutdown();
    volumeSRV_.Reset();
    volumeTexture_.Reset();
    particleSRVAdditive_.Reset();
    particleSRVAlpha_.Reset();
    particleBufferAdditive_.Reset();
    particleBufferAlpha_.Reset();
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
    const wchar_t* particlesFile = L"assets\\shaders\\particles.hlsl";
    const wchar_t* bloomFile = L"assets\\shaders\\bloom.hlsl";
    const wchar_t* compositeFile = L"assets\\shaders\\composite.hlsl";

    auto vsBlob = compile(skyFile, "FullscreenVS", "vs_5_0");
    auto skyBlob = compile(skyFile, "PSSky", "ps_5_0");
    auto groundBlob = compile(groundFile, "PSGround", "ps_5_0");
    auto volumeBlob = compile(volumeFile, "PSVolume", "ps_5_0");
    auto particleVSBlob = compile(particlesFile, "VSParticle", "vs_5_0");
    auto particleAddBlob = compile(particlesFile, "PSAdditive", "ps_5_0");
    auto particleAlphaBlob = compile(particlesFile, "PSAlpha", "ps_5_0");
    auto downsampleBlob = compile(bloomFile, "PSDownsample", "ps_5_0");
    auto upsampleBlob = compile(bloomFile, "PSUpsample", "ps_5_0");
    auto compositeBlob = compile(compositeFile, "PSComposite", "ps_5_0");

    if (!vsBlob || !skyBlob || !groundBlob || !volumeBlob || !particleVSBlob ||
        !particleAddBlob || !particleAlphaBlob || !downsampleBlob || !upsampleBlob ||
        !compositeBlob) {
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
        !createVS(particleVSBlob.Get(), particleVS_) ||
        !createPS(particleAddBlob.Get(), particleAdditivePS_) ||
        !createPS(particleAlphaBlob.Get(), particleAlphaPS_) ||
        !createPS(downsampleBlob.Get(), downsamplePS_) ||
        !createPS(upsampleBlob.Get(), upsamplePS_) ||
        !createPS(compositeBlob.Get(), compositePS_)) {
        LogError("Failed to create shader objects.");
        return false;
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

void Renderer::UploadVolume(const sim::VolumeFrame& frame) {
    if (frame.empty()) {
        volumeValid_ = false;
        return;
    }

    const bool split = frame.split;
    const bool halfFloat = frame.halfFloat;
    const DXGI_FORMAT format = split ? DXGI_FORMAT_R8G8_UNORM
                                     : (halfFloat ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM);

    if (!volumeTexture_ || volumeResolution_ != frame.resolution ||
        volumeHalfFloat_ != halfFloat || volumeSplit_ != split) {
        volumeSRV_.Reset();
        volumeTexture_.Reset();
        volumeAuxSRV_.Reset();
        volumeAuxTexture_.Reset();

        auto createVolume = [this](int resolution, DXGI_FORMAT textureFormat,
                                   ComPtr<ID3D11Texture3D>& texture,
                                   ComPtr<ID3D11ShaderResourceView>& srv) {
            D3D11_TEXTURE3D_DESC desc{};
            desc.Width = UINT(resolution);
            desc.Height = UINT(resolution);
            desc.Depth = UINT(resolution);
            desc.MipLevels = 1;
            desc.Format = textureFormat;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device_->Device()->CreateTexture3D(&desc, nullptr, &texture))) {
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = textureFormat;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            srvDesc.Texture3D.MipLevels = 1;
            return SUCCEEDED(
                device_->Device()->CreateShaderResourceView(texture.Get(), &srvDesc, &srv));
        };

        if (!createVolume(frame.resolution, format, volumeTexture_, volumeSRV_)) {
            LogError("Failed to create 3D volume texture at %d^3.", frame.resolution);
            volumeValid_ = false;
            return;
        }

        if (split) {
            if (!createVolume(frame.resolution, DXGI_FORMAT_R8G8_UNORM, volumeAuxTexture_,
                              volumeAuxSRV_)) {
                LogError("Failed to create the auxiliary 3D volume texture at %d^3.",
                         frame.resolution);
                volumeValid_ = false;
                return;
            }
        } else {
            volumeAuxSRV_ = volumeDummySRV_;
        }

        volumeResolution_ = frame.resolution;
        volumeHalfFloat_ = halfFloat;
        volumeSplit_ = split;
    }

    const UINT resolution = UINT(frame.resolution);
    const UINT rowPitch = resolution * UINT(frame.bytesPerVoxel);
    const UINT depthPitch = rowPitch * resolution;
    device_->Context()->UpdateSubresource(volumeTexture_.Get(), 0, nullptr, frame.rgba.data(),
                                          rowPitch, depthPitch);

    if (split && volumeAuxTexture_) {
        const UINT auxPitch = resolution * 2;
        const UINT auxDepthPitch = auxPitch * resolution;
        device_->Context()->UpdateSubresource(volumeAuxTexture_.Get(), 0, nullptr,
                                              frame.rgba2.data(), auxPitch, auxDepthPitch);
    }

    volumeValid_ = true;
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
    frameConstants_.volumeParams = XMFLOAT4(1.0f,
                                            activeVolumeSplit_ ? 1.0f : 0.0f,
                                            1.0f, 1.0f);
    frameConstants_.volumeParams2 =
        XMFLOAT4(highFidelity ? 0.40f : 1.0f, highFidelity ? 0.16f : 1.0f,
                 highFidelity ? 0.0f : 1.0f, 0.0f);

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
    config.domainHalfExtent = std::clamp(3.0 * fireball, 1.0, 30.0);
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

    constexpr double kTriggerFraction = 0.75;
    constexpr float kExpansion = 1.3f;
    constexpr float kMaxAccuracy = 16.0f;

    // The analytic model is only a fallback for the first reduction pass. It
    // consistently underestimates the rising Euler plume, which is exactly what
    // used to get clipped at the top of the domain.
    double requiredHalf = 0.0;
    double requiredTop = 0.0;
    if (solver_.HasFieldBounds()) {
        requiredHalf = std::max(double(solver_.FieldRadius()) * 1.10, 0.5);
        requiredTop = std::max(double(solver_.FieldTop()) * 1.10, 1.0);
    } else {
        const sim::BlastState& state = simulation.Blast().State();
        requiredHalf = std::max({state.fireballRadius * 2.4,
                                 state.cloudRadius * 1.35, 0.5});
        requiredTop =
            std::max({state.fireballCenterY + state.fireballRadius * 2.0,
                      state.cloudCenterY + state.cloudRadius * 1.6, 1.0});
    }

    // A single 1.3x step is not always enough after a fast plume rise, so keep
    // expanding until the measured field fits with margin (or the accuracy
    // budget is exhausted).
    for (int attempt = 0; attempt < 4; ++attempt) {
        const sim::VolumeBounds& bounds = solver_.Bounds();
        const double half = 0.5 * (bounds.max.x - bounds.min.x);
        const double top = bounds.max.y;
        const bool tooWide = requiredHalf > half * kTriggerFraction;
        const bool tooTall = requiredTop > top * kTriggerFraction;
        if (!tooWide && !tooTall) {
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

void Renderer::PrepareHighFidelity(sim::Simulation& simulation, double targetTime) {
    if (!solver_.Reset(MakeEulerConfig(simulation))) {
        LogError("High-fidelity solver could not be initialised.");
        solverPrepared_ = false;
        return;
    }
    solverBlastSerial_ = simulation.BlastSerial();
    solverCapWarned_ = false;

    const auto start = std::chrono::steady_clock::now();
    int guard = 0;
    while (solver_.Stats().time < targetTime && guard++ < 4096) {
        MaybeExpandSolver(simulation);
        solver_.Step(16);
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

    LogInfo("High-fidelity: t=%.4f s, %lld steps, %d^3, dt=%.2f us, maxSpeed=%.0f m/s",
            solver_.Stats().time, solver_.Stats().steps, solver_.Stats().resolution,
            solver_.Stats().dt * 1e6f, solver_.Stats().maxSpeed);
}

void Renderer::PrepareHighFidelitySteps(sim::Simulation& simulation, long long targetSteps) {
    if (!solver_.Reset(MakeEulerConfig(simulation))) {
        LogError("High-fidelity solver could not be initialised.");
        solverPrepared_ = false;
        return;
    }
    solverBlastSerial_ = simulation.BlastSerial();
    solverCapWarned_ = false;

    const auto start = std::chrono::steady_clock::now();
    while (solver_.Stats().steps < targetSteps) {
        MaybeExpandSolver(simulation);

        const long long remaining = targetSteps - solver_.Stats().steps;
        const int batch = int(std::min<long long>(remaining, 16));
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

    LogInfo("High-fidelity capture: %lld steps, t=%.4f s, domain half %.2f m, top %.2f m, "
            "accuracy x%.2f, expansions %d, plume top %.2f m, radius %.2f m",
            solver_.Stats().steps, solver_.Stats().time, solver_.Stats().domainHalfExtent,
            solver_.Stats().domainTop, solver_.Stats().accuracyFactor,
            solver_.Stats().expansions, solver_.Stats().plumeTop,
            solver_.Stats().plumeRadius);
}

double Renderer::ConsumeSolverAdvance() {    return solver_.ConsumeAdvance();
}

void Renderer::StepSolverOnce(int steps) {
    solver_.Step(steps);
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
        if (!simulation.IsArmed()) {
            if (solverBlastSerial_ != simulation.BlastSerial()) {
                if (solver_.Reset(MakeEulerConfig(simulation))) {
                    solverBlastSerial_ = simulation.BlastSerial();
                    solverCapWarned_ = false;
                } else {
                    LogWarn("High-fidelity solver reset failed.");
                }
                solverPrepared_ = false;
            }

            if (solver_.HasVolume()) {
                if (!solverPrepared_ && !solverPaused_) {
                    MaybeExpandSolver(simulation);
                    solver_.Step(std::clamp(simulation.Settings().solverStepsPerFrame, 1, 96));
                }
                solverVolume = solver_.VolumeSRV();
            }
        }
        solverPrepared_ = false;

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

    const int capacity = simulation.Particles().Capacity();
    if (capacity != particleCapacity_) {
        particleCapacity_ = capacity;
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

    if (!highFidelity) {
        UploadParticles(simulation.Particles().AdditiveBatch(), particleBufferAdditive_);
        UploadParticles(simulation.Particles().FireBatch(), particleBufferFire_);
        UploadParticles(simulation.Particles().AlphaBatch(), particleBufferAlpha_);
    }

    if (frameIndex_ == 0) {
        const auto& state = simulation.Blast().State();
        LogInfo("First frame: t=%.3fs fireball=%.2fm (%.0f K) shock=%.1fm volume=%d "
                "particles=%d+%d highFidelity=%d",
                state.time, state.fireballRadius, state.fireballTemperatureK, state.shockRadius,
                activeVolumeValid_ ? 1 : 0, int(simulation.Particles().AdditiveBatch().size()),
                int(simulation.Particles().AlphaBatch().size()), highFidelity ? 1 : 0);
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

    // --- Ground ------------------------------------------------------------
    if (settings.enableGround) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthWrite_.Get(), 0);
        context->PSSetShader(groundPS_.Get(), nullptr, 0);
        context->Draw(3, 0);
    }

    // Snapshot the depth buffer so subsequent passes can sample it without a
    // read/write hazard on the depth-stencil view.
    context->OMSetRenderTargets(1, &hdrRTV, nullptr);
    context->CopyResource(depthCopyTexture_.Get(), device_->DepthTexture());
    ID3D11ShaderResourceView* depthSample = depthCopySRV_.Get();

    info_.additiveParticles =
        highFidelity ? 0 : int(simulation.Particles().AdditiveBatch().size());
    info_.fireParticles = highFidelity ? 0 : int(simulation.Particles().FireBatch().size());
    info_.alphaParticles =
        highFidelity ? 0 : int(simulation.Particles().AlphaBatch().size());
    info_.raymarchSteps = settings.raymarchSteps;
    info_.volumeValid = activeVolumeValid_;
    info_.highFidelity = highFidelity;

    ID3D11ShaderResourceView* volumeView =
        highFidelity ? solverVolume : volumeSRV_.Get();

    // Hot debris and sparks are drawn before the volume so that the optically
    // thick fireball correctly occludes the material inside it.
    if (!highFidelity && settings.enableParticles && info_.additiveParticles > 0) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthTest_.Get(), 0);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(particleVS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {particleSRVAdditive_.Get(), depthSample};
        context->VSSetShaderResources(0, 1, srvs);
        context->PSSetShaderResources(0, 2, srvs);
        context->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(particleAdditivePS_.Get(), nullptr, 0);
        context->DrawInstanced(6, UINT(info_.additiveParticles), 0, 0);

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->VSSetShaderResources(0, 1, nullSRVs);
        context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // --- Volumetric fireball / smoke ---------------------------------------
    if (settings.enableVolume && activeVolumeValid_ && volumeView) {
        context->OMSetRenderTargets(1, &hdrRTV, nullptr);
        context->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[4] = {volumeView, depthSample, noiseSRV_.Get(),
                                             volumeAuxSRV_ ? volumeAuxSRV_.Get()
                                                           : volumeDummySRV_.Get()};
        context->PSSetShaderResources(0, 4, srvs);
        ID3D11SamplerState* samplers[2] = {linearClamp_.Get(), noiseSampler_.Get()};
        context->PSSetSamplers(0, 2, samplers);
        context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
        context->OMSetBlendState(blendPremultiplied_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(volumePS_.Get(), nullptr, 0);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRVs[4] = {nullptr, nullptr, nullptr, nullptr};
        context->PSSetShaderResources(0, 4, nullSRVs);
        context->PSSetSamplers(0, 1, linearClamp_.GetAddressOf());
    }

    // --- Particles ---------------------------------------------------------
    // Turbulent flame tongues on top of the fireball volume.
    if (!highFidelity && settings.enableParticles && info_.fireParticles > 0) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthTest_.Get(), 0);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(particleVS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {particleSRVFire_.Get(), depthSample};
        context->VSSetShaderResources(0, 1, srvs);
        context->PSSetShaderResources(0, 2, srvs);
        context->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(particleAdditivePS_.Get(), nullptr, 0);
        context->DrawInstanced(6, UINT(info_.fireParticles), 0, 0);

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->VSSetShaderResources(0, 1, nullSRVs);
        context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // Soft smoke and dust puffs composite over the volume.
    if (!highFidelity && settings.enableParticles && info_.alphaParticles > 0) {
        context->OMSetRenderTargets(1, &hdrRTV, device_->DepthDSV());
        context->OMSetDepthStencilState(depthTest_.Get(), 0);
        SetViewport(hdr_.width, hdr_.height);
        context->VSSetShader(particleVS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srvs[2] = {particleSRVAlpha_.Get(), depthSample};
        context->VSSetShaderResources(0, 1, srvs);
        context->PSSetShaderResources(0, 2, srvs);
        context->OMSetBlendState(blendAlpha_.Get(), nullptr, 0xffffffff);
        context->PSSetShader(particleAlphaPS_.Get(), nullptr, 0);
        context->DrawInstanced(6, UINT(info_.alphaParticles), 0, 0);

        ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
        context->VSSetShaderResources(0, 1, nullSRVs);
        context->PSSetShaderResources(0, 2, nullSRVs);
    }

    // --- Post processing ---------------------------------------------------
    if (settings.enableBloom) {
        RenderBloom();
    }

    device_->BindFinalTarget();
    context->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    context->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);

    bloomConstants_.bloomParams.z = settings.exposure;
    bloomConstants_.bloomParams.w = settings.bloomStrength;
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
