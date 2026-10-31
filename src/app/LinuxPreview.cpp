#include "app/LinuxPreview.h"

#include "core/Log.h"
#include "gfx/Camera.h"
#include "gfx/SceneMeshLibrary.h"
#include "scene/SceneTypes.h"
#include "scene/VehicleModels.h"
#include "sim/Simulation.h"

#include "BloomShaders.h"
#include "ChunkShaders.h"
#include "CompositeShaders.h"
#include "GroundShaders.h"
#include "ParticleExtraShaders.h"
#include "ParticleShaders.h"
#include "SceneShaders.h"
#include "ShockShaders.h"
#include "SkyShaders.h"
#include "VolumeShaders.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ex::app {

namespace {

using ex::gfx::SceneMeshVertex;

struct SceneInstance {
    float transform[16];
    float color[4];
    float params[4];
    float state[4];
    float textureOrigin[4];
    float textureScale[4];
};

struct MeshRange {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};

std::unique_ptr<ex::rhi::Texture> MakeSurfaceDetailTexture(
    ex::rhi::Device* device) {
    constexpr int kSize = 256;
    auto hash = [](int x, int y, int seed) {
        uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u +
                     uint32_t(seed) * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return float(h & 0x00ffffffu) / 16777215.0f;
    };
    auto wrap = [](int v, int period) {
        v %= period;
        return v < 0 ? v + period : v;
    };
    auto noise = [&hash, &wrap](float x, float y, int period) {
        const int x0 = int(std::floor(x));
        const int y0 = int(std::floor(y));
        const float fx = x - float(x0);
        const float fy = y - float(y0);
        const float sx = fx * fx * (3.0f - 2.0f * fx);
        const float sy = fy * fy * (3.0f - 2.0f * fy);
        const int xa = wrap(x0, period);
        const int ya = wrap(y0, period);
        const int xb = wrap(x0 + 1, period);
        const int yb = wrap(y0 + 1, period);
        const float n00 = hash(xa, ya, period);
        const float n10 = hash(xb, ya, period);
        const float n01 = hash(xa, yb, period);
        const float n11 = hash(xb, yb, period);
        const float nx0 = n00 + (n10 - n00) * sx;
        const float nx1 = n01 + (n11 - n01) * sx;
        return nx0 + (nx1 - nx0) * sy;
    };
    auto fbm = [&noise](float x, float y, int basePeriod, int seedOffset) {
        float value = 0.0f;
        float amplitude = 0.55f;
        float scale = 1.0f;
        int period = basePeriod;
        for (int octave = 0; octave < 5; ++octave) {
            value += amplitude * noise(x * scale + float(seedOffset),
                                       y * scale + float(seedOffset), period);
            amplitude *= 0.5f;
            scale *= 2.0f;
            period *= 2;
        }
        return std::clamp(value, 0.0f, 1.0f);
    };

    std::vector<uint8_t> base(size_t(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float u = float(x) / float(kSize);
            const float v = float(y) / float(kSize);
            const float grain = fbm(u * 8.0f, v * 8.0f, 8, 0);
            const float grime = fbm(u * 3.0f, v * 3.0f, 3, 17);
            const float speckle = hash(x, y, 91);
            const size_t index = (size_t(y) * kSize + size_t(x)) * 4;
            base[index + 0] =
                uint8_t(std::clamp(grain, 0.0f, 1.0f) * 255.0f);
            base[index + 1] =
                uint8_t(std::clamp(grime, 0.0f, 1.0f) * 255.0f);
            base[index + 2] = uint8_t(speckle * 255.0f);
            base[index + 3] = 255;
        }
    }

    std::vector<std::vector<uint8_t>> levels;
    levels.push_back(std::move(base));
    int width = kSize;
    while (width > 1) {
        const int nextWidth = width / 2;
        std::vector<uint8_t> next(size_t(nextWidth) * nextWidth * 4);
        const std::vector<uint8_t>& src = levels.back();
        for (int y = 0; y < nextWidth; ++y) {
            for (int x = 0; x < nextWidth; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const size_t index =
                                (size_t(y * 2 + dy) * width +
                                 size_t(x * 2 + dx)) *
                                    4 +
                                size_t(channel);
                            sum += src[index];
                        }
                    }
                    const size_t index =
                        (size_t(y) * nextWidth + size_t(x)) * 4 +
                        size_t(channel);
                    next[index] = uint8_t(sum / 4);
                }
            }
        }
        levels.push_back(std::move(next));
        width = nextWidth;
    }

    ex::rhi::TextureDesc desc{};
    desc.format = ex::rhi::Format::R8G8B8A8_UNORM;
    desc.width = kSize;
    desc.height = kSize;
    desc.mipLevels = uint32_t(levels.size());
    desc.usage = ex::rhi::TextureUsage::Sampled;
    std::vector<ex::rhi::TextureSubresource> subresources(levels.size());
    int levelWidth = kSize;
    for (size_t level = 0; level < levels.size(); ++level) {
        subresources[level].data = levels[level].data();
        subresources[level].width = uint32_t(levelWidth);
        subresources[level].height = uint32_t(levelWidth);
        subresources[level].rowPitch = uint32_t(levelWidth * 4);
        levelWidth = std::max(1, levelWidth / 2);
    }
    desc.subresources = subresources.data();
    desc.subresourceCount = uint32_t(subresources.size());
    return device->CreateTexture(desc);
}

} // namespace

bool LinuxPreview::Initialize(ex::rhi::Device* device, int width, int height) {
    if (!device) {
        return false;
    }
    device_ = device;
    if (!CreateTargets(device, width, height) || !CreateSky(device) ||
        !CreateGround(device) || !CreateScene(device) ||
        !CreateVolume(device) || !CreateParticles(device) ||
        !CreateShock(device) || !CreateComposite(device)) {
        return false;
    }
    return true;
}

void LinuxPreview::Shutdown() {
    compositeSet_.reset();
    compositePipeline_.reset();
    compositePS_.reset();
    compositeVS_.reset();
    bloomUpSet_.reset();
    bloomDownSet_.reset();
    bloomUpPipeline_.reset();
    bloomDownPipeline_.reset();
    bloomUpPS_.reset();
    bloomDownPS_.reset();
    bloomVS_.reset();
    bloomConstants_.reset();
    compositeSampler_.reset();
    bloomUpTarget_.reset();
    bloomDownTarget_.reset();
    bloomTexture_.reset();
    shockSet_.reset();
    shockPipeline_.reset();
    shockPS_.reset();
    shockVS_.reset();
    shockSampler_.reset();
    shockDepthProxy_.reset();
    shockTarget_.reset();
    volumeDepthTarget_.reset();
    particleSet_.reset();
    particlePipeline_.reset();
    particlePS_.reset();
    particleVS_.reset();
    particleInstanceBuffer_.reset();
    particleAlphaSet_.reset();
    particleAlphaPipeline_.reset();
    particleAlphaPS_.reset();
    particleAlphaBuffer_.reset();
    particleFireSet_.reset();
    particleFirePipeline_.reset();
    particleFirePS_.reset();
    particleFireBuffer_.reset();
    particleGpuFireSet_.reset();
    particleGpuAlphaSet_.reset();
    particleGpuAdditiveSet_.reset();
    gpuParticleIndirect_ = nullptr;
    gpuParticleFire_ = nullptr;
    gpuParticleAlpha_ = nullptr;
    gpuParticleAdditive_ = nullptr;
    volumeSet_.reset();
    volumePipeline_.reset();
    volumePS_.reset();
    volumeVS_.reset();
    surfaceSampler_.reset();
    volumeNoiseSampler_.reset();
    volumeDepthTexture_.reset();
    volumeAuxTexture_.reset();
    volumeLightTexture_.reset();
    volumeNoiseTexture_.reset();
    for (SceneDraw& draw : sceneDraws_) {
        draw.set.reset();
    }
    sceneDraws_.clear();
    scenePipeline_.reset();
    scenePS_.reset();
    sceneVS_.reset();
    chunkSet_.reset();
    chunkPipeline_.reset();
    chunkVS_.reset();
    chunkInstanceBuffer_.reset();
    chunkIndexBuffer_.reset();
    chunkVertexBuffer_.reset();
    surfaceTexture_.reset();
    sceneCascadeBuffer_.reset();
    sceneIndexBuffer_.reset();
    sceneVertexBuffer_.reset();
    groundSet_.reset();
    groundPipeline_.reset();
    groundPS_.reset();
    groundVS_.reset();
    volumeSampler_.reset();
    shadowSampler_.reset();
    volumeTexture_.reset();
    for (auto& texture : shadowTextures_) {
        texture.reset();
    }
    shadowConstants_.reset();
    skySet_.reset();
    skyPipeline_.reset();
    skyPS_.reset();
    skyVS_.reset();
    frameConstants_.reset();
    depthTarget_.reset();
    hdrTarget_.reset();
    commandList_.reset();
}

bool LinuxPreview::CreateTargets(ex::rhi::Device* device, int width,
                                 int height) {
    ex::rhi::TextureDesc hdrDesc{};
    hdrDesc.format = ex::rhi::Format::B8G8R8A8_UNORM;
    hdrDesc.width = uint32_t(std::max(width, 1));
    hdrDesc.height = uint32_t(std::max(height, 1));
    hdrDesc.usage = ex::rhi::TextureUsage::RenderTarget |
                    ex::rhi::TextureUsage::Sampled;
    hdrTarget_ = device->CreateTexture(hdrDesc);

    ex::rhi::TextureDesc depthDesc{};
    depthDesc.format = ex::rhi::Format::D32_FLOAT;
    depthDesc.width = hdrDesc.width;
    depthDesc.height = hdrDesc.height;
    depthDesc.usage = ex::rhi::TextureUsage::DepthStencil |
                      ex::rhi::TextureUsage::Sampled;
    depthTarget_ = device->CreateTexture(depthDesc);

    ex::rhi::TextureDesc volumeDepthAttachmentDesc{};
    volumeDepthAttachmentDesc.format = ex::rhi::Format::D32_FLOAT;
    volumeDepthAttachmentDesc.width = hdrDesc.width;
    volumeDepthAttachmentDesc.height = hdrDesc.height;
    volumeDepthAttachmentDesc.usage = ex::rhi::TextureUsage::DepthStencil;
    volumeDepthTarget_ = device->CreateTexture(volumeDepthAttachmentDesc);

    ex::rhi::BufferDesc frameDesc{};
    frameDesc.size = sizeof(frameConstantsData_);
    frameDesc.usage = ex::rhi::BufferUsage::Uniform |
                      ex::rhi::BufferUsage::Dynamic;
    frameDesc.initialData = frameConstantsData_;
    frameConstants_ = device->CreateBuffer(frameDesc);

    float shadow[80] = {};
    for (int matrix = 0; matrix < 4; ++matrix) {
        const int base = matrix * 16;
        shadow[base + 0] = 1.0f;
        shadow[base + 5] = 1.0f;
        shadow[base + 10] = 1.0f;
        shadow[base + 15] = 1.0f;
    }
    shadow[64] = 10.0f;
    shadow[65] = 30.0f;
    shadow[66] = 100.0f;
    shadow[68] = 1.0f / 2048.0f;
    shadow[69] = 1.0f / 1024.0f;
    shadow[70] = 1.0f / 1024.0f;
    shadow[72] = 0.1f;
    shadow[73] = 0.1f;
    shadow[74] = 0.1f;
    shadow[76] = 1.0f / 4096.0f;
    shadow[77] = 1.0f / 4096.0f;
    shadow[78] = 0.0004f;
    ex::rhi::BufferDesc shadowDesc{};
    shadowDesc.size = sizeof(shadow);
    shadowDesc.usage = ex::rhi::BufferUsage::Uniform |
                       ex::rhi::BufferUsage::Dynamic;
    shadowDesc.initialData = shadow;
    shadowConstants_ = device->CreateBuffer(shadowDesc);

    const float lit[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    ex::rhi::TextureSubresource shadowSubresource{};
    shadowSubresource.data = lit;
    shadowSubresource.width = 2;
    shadowSubresource.height = 2;
    shadowSubresource.rowPitch = 8;
    ex::rhi::TextureDesc shadowDescTexture{};
    shadowDescTexture.format = ex::rhi::Format::D32_FLOAT;
    shadowDescTexture.width = 2;
    shadowDescTexture.height = 2;
    shadowDescTexture.usage = ex::rhi::TextureUsage::Sampled |
                              ex::rhi::TextureUsage::DepthStencil;
    shadowDescTexture.subresources = &shadowSubresource;
    shadowDescTexture.subresourceCount = 1;
    for (auto& texture : shadowTextures_) {
        texture = device->CreateTexture(shadowDescTexture);
    }

    const int volumeSize = 8;
    std::vector<uint8_t> volumePixels(
        size_t(volumeSize) * size_t(volumeSize) * size_t(volumeSize) * 4, 0);
    for (int z = 0; z < volumeSize; ++z) {
        for (int y = 0; y < volumeSize; ++y) {
            for (int x = 0; x < volumeSize; ++x) {
                const size_t index =
                    ((size_t(z) * size_t(volumeSize) + size_t(y)) *
                         size_t(volumeSize) +
                     size_t(x)) *
                    4;
                volumePixels[index + 0] = 2;
                volumePixels[index + 1] = 200;
                volumePixels[index + 2] = 80;
                volumePixels[index + 3] = 40;
            }
        }
    }
    ex::rhi::TextureSubresource volumeSubresource{};
    volumeSubresource.data = volumePixels.data();
    volumeSubresource.width = uint32_t(volumeSize);
    volumeSubresource.height = uint32_t(volumeSize);
    volumeSubresource.rowPitch = uint32_t(volumeSize) * 4;
    volumeSubresource.slicePitch =
        uint32_t(volumeSize) * uint32_t(volumeSize) * 4;
    ex::rhi::TextureDesc volumeDesc{};
    volumeDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
    volumeDesc.width = uint32_t(volumeSize);
    volumeDesc.height = uint32_t(volumeSize);
    volumeDesc.depth = uint32_t(volumeSize);
    volumeDesc.usage = ex::rhi::TextureUsage::Sampled;
    volumeDesc.subresources = &volumeSubresource;
    volumeDesc.subresourceCount = 1;
    volumeTexture_ = device->CreateTexture(volumeDesc);
    volumeAuxTexture_ = device->CreateTexture(volumeDesc);

    std::vector<uint8_t> noisePixels(volumePixels.size());
    for (size_t i = 0; i < noisePixels.size(); ++i) {
        noisePixels[i] = uint8_t((i * 37) & 0xff);
    }
    ex::rhi::TextureSubresource noiseSubresource = volumeSubresource;
    noiseSubresource.data = noisePixels.data();
    ex::rhi::TextureDesc noiseDesc = volumeDesc;
    noiseDesc.subresources = &noiseSubresource;
    volumeNoiseTexture_ = device->CreateTexture(noiseDesc);

    std::vector<uint8_t> lightPixels(volumePixels.size(), 255);
    ex::rhi::TextureSubresource lightSubresource = volumeSubresource;
    lightSubresource.data = lightPixels.data();
    ex::rhi::TextureDesc lightDesc = volumeDesc;
    lightDesc.subresources = &lightSubresource;
    volumeLightTexture_ = device->CreateTexture(lightDesc);

    // The RHI has no depth-copy path yet, so the volume pass samples a far
    // depth placeholder. A value of 255 keeps the raymarch from being clamped
    // to the near range, which hid the solver volume behind the town.
    std::vector<uint8_t> volumeDepthPixels(
        size_t(std::max(width, 1)) * size_t(std::max(height, 1)) * 4, 255);
    ex::rhi::TextureSubresource volumeDepthSubresource{};
    volumeDepthSubresource.data = volumeDepthPixels.data();
    volumeDepthSubresource.width = uint32_t(std::max(width, 1));
    volumeDepthSubresource.height = uint32_t(std::max(height, 1));
    volumeDepthSubresource.rowPitch = uint32_t(std::max(width, 1)) * 4;
    ex::rhi::TextureDesc volumeDepthDesc{};
    volumeDepthDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
    volumeDepthDesc.width = uint32_t(std::max(width, 1));
    volumeDepthDesc.height = uint32_t(std::max(height, 1));
    volumeDepthDesc.usage = ex::rhi::TextureUsage::Sampled;
    volumeDepthDesc.subresources = &volumeDepthSubresource;
    volumeDepthDesc.subresourceCount = 1;
    volumeDepthTexture_ = device->CreateTexture(volumeDepthDesc);

    ex::rhi::SamplerDesc shadowSamplerDesc{};
    shadowSamplerDesc.comparison = true;
    shadowSamplerDesc.linear = true;
    shadowSamplerDesc.border[0] = 1.0f;
    shadowSamplerDesc.border[1] = 1.0f;
    shadowSamplerDesc.border[2] = 1.0f;
    shadowSamplerDesc.border[3] = 1.0f;
    shadowSampler_ = device->CreateSampler(shadowSamplerDesc);

    ex::rhi::SamplerDesc volumeSamplerDesc{};
    volumeSamplerDesc.linear = true;
    volumeSamplerDesc.clamp = true;
    volumeSampler_ = device->CreateSampler(volumeSamplerDesc);
    volumeNoiseSampler_ = device->CreateSampler(volumeSamplerDesc);

    ex::rhi::SamplerDesc surfaceSamplerDesc{};
    surfaceSamplerDesc.linear = true;
    surfaceSamplerDesc.wrap = true;
    surfaceSamplerDesc.maxAnisotropy = 8;
    surfaceSampler_ = device->CreateSampler(surfaceSamplerDesc);

    surfaceTexture_ = MakeSurfaceDetailTexture(device);

    float cascade[16] = {};
    cascade[0] = 1.0f;
    cascade[5] = 1.0f;
    cascade[10] = 1.0f;
    cascade[15] = 1.0f;
    ex::rhi::BufferDesc cascadeDesc{};
    cascadeDesc.size = sizeof(cascade);
    cascadeDesc.usage = ex::rhi::BufferUsage::Uniform |
                        ex::rhi::BufferUsage::Dynamic;
    cascadeDesc.initialData = cascade;
    sceneCascadeBuffer_ = device->CreateBuffer(cascadeDesc);

    const uint8_t bloomPixels[16] = {
        16, 16, 16, 255, 16, 16, 16, 255,
        16, 16, 16, 255, 16, 16, 16, 255,
    };
    ex::rhi::TextureSubresource bloomSubresource{};
    bloomSubresource.data = bloomPixels;
    bloomSubresource.width = 2;
    bloomSubresource.height = 2;
    bloomSubresource.rowPitch = 8;
    ex::rhi::TextureDesc bloomDesc{};
    bloomDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
    bloomDesc.width = 2;
    bloomDesc.height = 2;
    bloomDesc.usage = ex::rhi::TextureUsage::Sampled;
    bloomDesc.subresources = &bloomSubresource;
    bloomDesc.subresourceCount = 1;
    bloomTexture_ = device->CreateTexture(bloomDesc);
    bloomDownTarget_ = device->CreateTexture(hdrDesc);
    bloomUpTarget_ = device->CreateTexture(hdrDesc);
    shockTarget_ = device->CreateTexture(hdrDesc);

    std::vector<uint8_t> shockDepthPixels(
        size_t(std::max(width, 1)) * size_t(std::max(height, 1)) * 4, 230);
    ex::rhi::TextureSubresource shockDepthSubresource{};
    shockDepthSubresource.data = shockDepthPixels.data();
    shockDepthSubresource.width = uint32_t(std::max(width, 1));
    shockDepthSubresource.height = uint32_t(std::max(height, 1));
    shockDepthSubresource.rowPitch = uint32_t(std::max(width, 1)) * 4;
    ex::rhi::TextureDesc shockDepthDesc{};
    shockDepthDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
    shockDepthDesc.width = uint32_t(std::max(width, 1));
    shockDepthDesc.height = uint32_t(std::max(height, 1));
    shockDepthDesc.usage = ex::rhi::TextureUsage::Sampled;
    shockDepthDesc.subresources = &shockDepthSubresource;
    shockDepthDesc.subresourceCount = 1;
    shockDepthProxy_ = device->CreateTexture(shockDepthDesc);

    bloomConstantsData_[0] = 1.0f / float(std::max(width, 1));
    bloomConstantsData_[1] = 1.0f / float(std::max(height, 1));
    bloomConstantsData_[2] = bloomConstantsData_[0];
    bloomConstantsData_[3] = bloomConstantsData_[1];
    bloomConstantsData_[4] = 0.5f;
    bloomConstantsData_[5] = 1.0f;
    bloomConstantsData_[6] = settings_.exposure;
    bloomConstantsData_[7] = settings_.bloomStrength;
    ex::rhi::BufferDesc bloomDescBuffer{};
    bloomDescBuffer.size = sizeof(bloomConstantsData_);
    bloomDescBuffer.usage = ex::rhi::BufferUsage::Uniform |
                            ex::rhi::BufferUsage::Dynamic;
    bloomDescBuffer.initialData = bloomConstantsData_;
    bloomConstants_ = device->CreateBuffer(bloomDescBuffer);

    commandList_ = device->CreateCommandList();
    if (volumeDepthTarget_ && commandList_) {
        commandList_->BeginCompute();
        commandList_->TransitionTexture(
            volumeDepthTarget_.get(),
            ex::rhi::TextureState::DepthAttachment);
        commandList_->EndCompute();
    }
    bool ok = hdrTarget_ && depthTarget_ && volumeDepthTarget_ &&
              frameConstants_ &&
              shadowConstants_ && volumeTexture_ && volumeAuxTexture_ &&
              volumeNoiseTexture_ && volumeLightTexture_ &&
              volumeDepthTexture_ && volumeNoiseSampler_ &&
              shadowSampler_ && volumeSampler_ && surfaceSampler_ &&
              surfaceTexture_ &&
              sceneCascadeBuffer_ && bloomTexture_ && bloomDownTarget_ &&
              bloomUpTarget_ && shockTarget_ && shockDepthProxy_ &&
              bloomConstants_ && commandList_;
    for (const auto& texture : shadowTextures_) {
        ok = ok && texture;
    }
    return ok;
}

bool LinuxPreview::CreateSky(ex::rhi::Device* device) {
    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kSkyVS;
    shaderDesc.size = kSkyVSSize;
    shaderDesc.name = "FullscreenVS";
    skyVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kSkyPS;
    shaderDesc.size = kSkyPSSize;
    shaderDesc.name = "PSSky";
    skyPS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = skyVS_.get();
    pipelineDesc.pixelShader = skyPS_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 1;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    skyPipeline_ = device->CreatePipeline(pipelineDesc);
    if (skyPipeline_) {
        skySet_ = device->CreateDescriptorSet(skyPipeline_.get());
    }
    if (skySet_) {
        skySet_->SetBuffer(0, frameConstants_.get(), 0,
                           frameConstants_->Size());
    }
    return skyVS_ && skyPS_ && skyPipeline_ && skySet_;
}

bool LinuxPreview::CreateGround(ex::rhi::Device* device) {
    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kGroundVS;
    shaderDesc.size = kGroundVSSize;
    shaderDesc.name = "FullscreenVS";
    groundVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kGroundPS;
    shaderDesc.size = kGroundPSSize;
    shaderDesc.name = "PSGround";
    groundPS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
        {2, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
        {9, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {10, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {11, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {12, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {13, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {14, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {18, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
        {19, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = groundVS_.get();
    pipelineDesc.pixelShader = groundPS_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 10;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = ex::rhi::CompareOp::LessEqual;
    groundPipeline_ = device->CreatePipeline(pipelineDesc);
    if (groundPipeline_) {
        groundSet_ = device->CreateDescriptorSet(groundPipeline_.get());
    }
    if (groundSet_) {
        groundSet_->SetBuffer(0, frameConstants_.get(), 0,
                              frameConstants_->Size());
        groundSet_->SetBuffer(2, shadowConstants_.get(), 0,
                              shadowConstants_->Size());
        groundSet_->SetImage(9, volumeDepthTexture_.get());
        for (int i = 0; i < 4; ++i) {
            groundSet_->SetImage(uint32_t(10 + i), shadowTextures_[i].get());
        }
        groundSet_->SetImage(14, volumeTexture_.get());
        groundSet_->SetSampler(18, shadowSampler_.get());
        groundSet_->SetSampler(19, volumeSampler_.get());
    }
    return groundVS_ && groundPS_ && groundPipeline_ && groundSet_;
}

bool LinuxPreview::CreateScene(ex::rhi::Device* device) {
    const ex::gfx::SceneMeshData buildingMesh = ex::gfx::MakeBuildingMesh();
    const ex::gfx::SceneMeshData vehicleMesh =
        ex::gfx::MakeVehiclePlaceholderMesh();
    const ex::gfx::SceneMeshData treeMesh = ex::gfx::MakeTreeMesh(0);

    std::vector<SceneMeshVertex> vertices;
    std::vector<uint32_t> indices;
    auto appendMesh = [&](const ex::gfx::SceneMeshData& mesh) {
        const uint32_t baseVertex = uint32_t(vertices.size());
        const uint32_t indexOffset = uint32_t(indices.size());
        vertices.insert(vertices.end(), mesh.vertices.begin(),
                        mesh.vertices.end());
        for (uint32_t index : mesh.indices) {
            indices.push_back(baseVertex + index);
        }
        return MeshRange{indexOffset, uint32_t(mesh.indices.size())};
    };
    buildingRange_ = appendMesh(buildingMesh);
    vehicleRange_ = appendMesh(vehicleMesh);
    treeRange_ = appendMesh(treeMesh);

    ex::rhi::BufferDesc vertexDesc{};
    vertexDesc.size = vertices.size() * sizeof(SceneMeshVertex);
    vertexDesc.usage = ex::rhi::BufferUsage::Vertex;
    vertexDesc.initialData = vertices.data();
    sceneVertexBuffer_ = device->CreateBuffer(vertexDesc);

    ex::rhi::BufferDesc indexDesc{};
    indexDesc.size = indices.size() * sizeof(uint32_t);
    indexDesc.usage = ex::rhi::BufferUsage::Index;
    indexDesc.initialData = indices.data();
    sceneIndexBuffer_ = device->CreateBuffer(indexDesc);

    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kSceneVS;
    shaderDesc.size = kSceneVSSize;
    shaderDesc.name = "VSScene";
    sceneVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kScenePS;
    shaderDesc.size = kScenePSSize;
    shaderDesc.name = "PSScene";
    scenePS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel, 3},
        {2, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
        {3, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
        {8, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Vertex},
        {9, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Vertex},
        {10, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {11, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {12, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {13, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {14, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {15, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {18, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
        {19, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
        {20, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::VertexAttribute attributes[] = {
        {"POSITION", 0, ex::rhi::VertexFormat::Float3, 0, 0},
        {"NORMAL", 0, ex::rhi::VertexFormat::Float3, 12, 0},
        {"COLOR", 0, ex::rhi::VertexFormat::Float3, 24, 0},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = sceneVS_.get();
    pipelineDesc.pixelShader = scenePS_.get();
    pipelineDesc.vertexAttributes = attributes;
    pipelineDesc.vertexAttributeCount = 3;
    pipelineDesc.vertexStride = sizeof(SceneMeshVertex);
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 14;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompare = ex::rhi::CompareOp::LessEqual;
    pipelineDesc.cullMode = ex::rhi::CullMode::None;
    scenePipeline_ = device->CreatePipeline(pipelineDesc);

    ex::rhi::ShaderDesc chunkShaderDesc{};
    chunkShaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    chunkShaderDesc.data = kSceneVSChunk;
    chunkShaderDesc.size = kSceneVSChunkSize;
    chunkShaderDesc.name = "VSChunk";
    chunkVS_ = device->CreateShader(chunkShaderDesc);

    ex::rhi::VertexAttribute chunkAttributes[] = {
        {"POSITION", 0, ex::rhi::VertexFormat::Float3, 0, 0},
        {"NORMAL", 0, ex::rhi::VertexFormat::Float3, 12, 0},
        {"CHUNKID", 0, ex::rhi::VertexFormat::UInt, 24, 0},
    };
    ex::rhi::PipelineDesc chunkPipelineDesc = pipelineDesc;
    chunkPipelineDesc.vertexShader = chunkVS_.get();
    chunkPipelineDesc.vertexAttributes = chunkAttributes;
    chunkPipelineDesc.vertexAttributeCount = 3;
    chunkPipelineDesc.vertexStride = sizeof(ChunkVertex);
    chunkPipeline_ = device->CreatePipeline(chunkPipelineDesc);

    return sceneVertexBuffer_ && sceneIndexBuffer_ && sceneVS_ && scenePS_ &&
           scenePipeline_;
}

bool LinuxPreview::SetScene(const ex::scene::Scene& scene) {
    if (!device_) {
        return false;
    }
    device_->WaitIdle();
    sceneDraws_.clear();

    std::vector<SceneInstance> buildingInstances;
    std::vector<SceneInstance> vehicleInstances;
    std::vector<SceneInstance> treeInstances;
    for (const ex::scene::SceneObject& object : scene.Objects()) {
        if (object.collapsed) {
            continue;
        }
        SceneInstance item{};
        DirectX::XMMATRIX world;
        if (object.type == ex::scene::SceneObjectType::Tree) {
            world = DirectX::XMMatrixScaling(object.size.x * 2.0f, object.size.y,
                                             object.size.x * 2.0f) *
                    DirectX::XMMatrixRotationY(
                        DirectX::XMConvertToRadians(object.yaw)) *
                    DirectX::XMMatrixTranslation(object.position.x, 0.0f,
                                                 object.position.z);
            item.params[3] = 2.0f;
            item.textureScale[3] = float(object.meshVariant & 3);
        } else if (object.type == ex::scene::SceneObjectType::Vehicle) {
            const float uniform =
                object.size.z > 1e-4f ? object.size.z / 2.8f : 1.0f;
            world = DirectX::XMMatrixScaling(uniform, uniform, uniform) *
                    DirectX::XMMatrixRotationY(
                        DirectX::XMConvertToRadians(object.yaw)) *
                    DirectX::XMMatrixTranslation(object.position.x, 0.0f,
                                                 object.position.z);
            item.params[3] = 1.0f;
            item.textureScale[3] =
                10.0f + float(object.meshVariant % ex::scene::kVehicleModelCount);
        } else {
            world = DirectX::XMMatrixScaling(object.size.x, object.size.y,
                                             object.size.z) *
                    DirectX::XMMatrixRotationY(
                        DirectX::XMConvertToRadians(object.yaw)) *
                    DirectX::XMMatrixTranslation(object.position.x,
                                                 object.position.y,
                                                 object.position.z);
            item.params[3] = 0.0f;
            item.textureScale[3] = float(object.meshVariant & 3);
        }
        DirectX::XMFLOAT4X4 matrix{};
        DirectX::XMStoreFloat4x4(&matrix, world);
        std::memcpy(item.transform, &matrix, sizeof(matrix));
        item.color[0] = object.color.x;
        item.color[1] = object.color.y;
        item.color[2] = object.color.z;
        item.color[3] = object.color.w;
        item.textureOrigin[0] = object.position.x;
        item.textureOrigin[2] = object.position.z;
        item.textureScale[0] = object.size.x;
        item.textureScale[1] = object.size.y;
        item.textureScale[2] = object.size.z;
        if (object.type == ex::scene::SceneObjectType::Tree) {
            treeInstances.push_back(item);
        } else if (object.type == ex::scene::SceneObjectType::Vehicle) {
            vehicleInstances.push_back(item);
        } else {
            buildingInstances.push_back(item);
        }
    }

    auto makeInstanceBuffer = [&](const std::vector<SceneInstance>& data) {
        ex::rhi::BufferDesc desc{};
        desc.size = std::max<size_t>(1, data.size()) * sizeof(SceneInstance);
        desc.usage = ex::rhi::BufferUsage::Storage |
                     ex::rhi::BufferUsage::ShaderRead |
                     ex::rhi::BufferUsage::Dynamic;
        desc.structureStride = sizeof(SceneInstance);
        desc.initialData = data.empty() ? nullptr : data.data();
        return device_->CreateBuffer(desc);
    };

    auto addDraw = [&](const MeshRange& range,
                       const std::vector<SceneInstance>& data) {
        if (data.empty() || !scenePipeline_) {
            return;
        }
        std::unique_ptr<ex::rhi::Buffer> instanceBuffer =
            makeInstanceBuffer(data);
        if (!instanceBuffer) {
            return;
        }
        std::unique_ptr<ex::rhi::DescriptorSet> set =
            device_->CreateDescriptorSet(scenePipeline_.get());
        if (!set) {
            return;
        }
        set->SetBuffer(0, frameConstants_.get(), 0, frameConstants_->Size());
        set->SetBuffer(2, shadowConstants_.get(), 0,
                       shadowConstants_->Size());
        set->SetBuffer(3, sceneCascadeBuffer_.get(), 0,
                       sceneCascadeBuffer_->Size());
        set->SetBuffer(8, instanceBuffer.get(), 0, instanceBuffer->Size());
        set->SetBuffer(9, instanceBuffer.get(), 0, instanceBuffer->Size());
        for (int i = 0; i < 4; ++i) {
            set->SetImage(uint32_t(10 + i), shadowTextures_[i].get());
        }
        set->SetImage(14, surfaceTexture_.get());
        set->SetImage(15, volumeTexture_.get());
        set->SetSampler(18, shadowSampler_.get());
        set->SetSampler(19, surfaceSampler_.get());
        set->SetSampler(20, volumeSampler_.get());

        SceneDraw draw;
        draw.instanceBuffer = std::move(instanceBuffer);
        draw.set = std::move(set);
        draw.indexOffset = range.indexOffset;
        draw.indexCount = range.indexCount;
        draw.instanceCount = uint32_t(data.size());
        sceneDraws_.push_back(std::move(draw));
    };
    addDraw(buildingRange_, buildingInstances);
    addDraw(vehicleRange_, vehicleInstances);
    addDraw(treeRange_, treeInstances);

    LogInfo("Preview scene: buildings=%zu vehicles=%zu trees=%zu",
            buildingInstances.size(), vehicleInstances.size(),
            treeInstances.size());
    if (!BuildChunks(scene)) {
        return false;
    }
    return !sceneDraws_.empty();
}

bool LinuxPreview::BuildChunks(const ex::scene::Scene& scene) {
    chunkDraws_.clear();
    chunkVertexBuffer_.reset();
    chunkIndexBuffer_.reset();
    chunkInstanceBuffer_.reset();
    chunkSet_.reset();
    chunkInstanceCount_ = 0;
    if (!device_ || !chunkPipeline_ ||
        !scene.Settings().voronoiChunks) {
        return true;
    }

    std::vector<ChunkVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SceneInstance> instances;
    const auto& objects = scene.Objects();
    const auto& bodies = scene.Physics().Bodies();
    uint32_t chunkId = 0;
    for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ex::scene::SceneObject& object = objects[objectIndex];
        if (object.chunks.empty() ||
            object.chunks.front().hullPositions.empty()) {
            continue;
        }
        ChunkDraw draw;
        draw.indexOffset = uint32_t(indices.size());
        draw.objectIndex = int(objectIndex);
        for (const ex::scene::SceneChunk& chunk : object.chunks) {
            if (chunk.hullPositions.empty() ||
                chunk.hullNormals.size() != chunk.hullPositions.size()) {
                continue;
            }
            const uint32_t id = chunkId++;
            const uint32_t base = uint32_t(vertices.size());
            for (size_t v = 0; v < chunk.hullPositions.size(); ++v) {
                vertices.push_back(
                    {chunk.hullPositions[v], chunk.hullNormals[v], id});
            }
            for (uint32_t index : chunk.hullIndices) {
                indices.push_back(base + index);
            }

            SceneInstance item{};
            DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
            if (chunk.bodyIndex >= 0 &&
                chunk.bodyIndex < int(bodies.size())) {
                const ex::scene::RigidBody& body = bodies[chunk.bodyIndex];
                const DirectX::XMMATRIX initial =
                    DirectX::XMMatrixRotationQuaternion(
                        DirectX::XMLoadFloat4(&chunk.orientation));
                const DirectX::XMMATRIX current =
                    DirectX::XMMatrixRotationQuaternion(
                        DirectX::XMLoadFloat4(&body.orientation));
                world = DirectX::XMMatrixTranslation(
                            -chunk.center.x, -chunk.center.y, -chunk.center.z) *
                        DirectX::XMMatrixTranspose(initial) * current *
                        DirectX::XMMatrixTranslation(body.position.x,
                                                     body.position.y,
                                                     body.position.z);
            }
            DirectX::XMFLOAT4X4 matrix{};
            DirectX::XMStoreFloat4x4(&matrix, world);
            std::memcpy(item.transform, &matrix, sizeof(matrix));
            item.color[0] = chunk.color.x;
            item.color[1] = chunk.color.y;
            item.color[2] = chunk.color.z;
            item.color[3] = chunk.color.w;
            item.params[0] = object.damageAmount * 0.7f;
            item.params[3] = 3.0f;
            item.textureScale[0] = 1.0f;
            item.textureScale[1] = 1.0f;
            item.textureScale[2] = 1.0f;
            item.textureScale[3] = float(object.meshVariant & 3);
            instances.push_back(item);
        }
        draw.indexCount = uint32_t(indices.size()) - draw.indexOffset;
        if (draw.indexCount > 0) {
            chunkDraws_.push_back(draw);
        }
    }
    if (vertices.empty() || indices.empty()) {
        return true;
    }

    ex::rhi::BufferDesc vertexDesc{};
    vertexDesc.size = vertices.size() * sizeof(ChunkVertex);
    vertexDesc.usage = ex::rhi::BufferUsage::Vertex;
    vertexDesc.initialData = vertices.data();
    chunkVertexBuffer_ = device_->CreateBuffer(vertexDesc);

    ex::rhi::BufferDesc indexDesc{};
    indexDesc.size = indices.size() * sizeof(uint32_t);
    indexDesc.usage = ex::rhi::BufferUsage::Index;
    indexDesc.initialData = indices.data();
    chunkIndexBuffer_ = device_->CreateBuffer(indexDesc);

    ex::rhi::BufferDesc instanceDesc{};
    instanceDesc.size =
        std::max<size_t>(1, instances.size()) * sizeof(SceneInstance);
    instanceDesc.usage = ex::rhi::BufferUsage::Storage |
                         ex::rhi::BufferUsage::ShaderRead |
                         ex::rhi::BufferUsage::Dynamic;
    instanceDesc.structureStride = sizeof(SceneInstance);
    instanceDesc.initialData =
        instances.empty() ? nullptr : instances.data();
    chunkInstanceBuffer_ = device_->CreateBuffer(instanceDesc);

    if (chunkPipeline_) {
        chunkSet_ = device_->CreateDescriptorSet(chunkPipeline_.get());
    }
    if (chunkSet_ && chunkInstanceBuffer_) {
        chunkSet_->SetBuffer(0, frameConstants_.get(), 0,
                             frameConstants_->Size());
        chunkSet_->SetBuffer(2, shadowConstants_.get(), 0,
                             shadowConstants_->Size());
        chunkSet_->SetBuffer(3, sceneCascadeBuffer_.get(), 0,
                             sceneCascadeBuffer_->Size());
        chunkSet_->SetBuffer(8, chunkInstanceBuffer_.get(), 0,
                             chunkInstanceBuffer_->Size());
        chunkSet_->SetBuffer(9, chunkInstanceBuffer_.get(), 0,
                             chunkInstanceBuffer_->Size());
        for (int i = 0; i < 4; ++i) {
            chunkSet_->SetImage(uint32_t(10 + i), shadowTextures_[i].get());
        }
        chunkSet_->SetImage(14, surfaceTexture_.get());
        chunkSet_->SetImage(15, volumeTexture_.get());
        chunkSet_->SetSampler(18, shadowSampler_.get());
        chunkSet_->SetSampler(19, surfaceSampler_.get());
        chunkSet_->SetSampler(20, volumeSampler_.get());
    }
    chunkInstanceCount_ = uint32_t(instances.size());
    LogInfo("Preview chunks: %u chunks in %zu objects", chunkInstanceCount_,
            chunkDraws_.size());
    return chunkVertexBuffer_ && chunkIndexBuffer_ && chunkInstanceBuffer_ &&
           chunkSet_;
}

bool LinuxPreview::CreateVolume(ex::rhi::Device* device) {


    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kVolumeVS;
    shaderDesc.size = kVolumeVSSize;
    shaderDesc.name = "FullscreenVS";
    volumeVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kVolumePS;
    shaderDesc.size = kVolumePSSize;
    shaderDesc.name = "PSVolume";
    volumePS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
        {8, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {9, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {10, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {11, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {12, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {16, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
        {17, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = volumeVS_.get();
    pipelineDesc.pixelShader = volumePS_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 8;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.blendEnable = true;
    pipelineDesc.premultipliedBlend = true;
    volumePipeline_ = device->CreatePipeline(pipelineDesc);
    if (volumePipeline_) {
        volumeSet_ = device->CreateDescriptorSet(volumePipeline_.get());
    }
    if (volumeSet_) {
        volumeSet_->SetBuffer(0, frameConstants_.get(), 0,
                              frameConstants_->Size());
        volumeSet_->SetImage(8, volumeTexture_.get());
        volumeSet_->SetImage(9, volumeDepthTexture_.get());
        volumeSet_->SetImage(10, volumeNoiseTexture_.get());
        volumeSet_->SetImage(11, volumeAuxTexture_.get());
        volumeSet_->SetImage(12, volumeLightTexture_.get());
        volumeSet_->SetSampler(16, volumeSampler_.get());
        volumeSet_->SetSampler(17, volumeNoiseSampler_.get());
    }
    return volumeVS_ && volumePS_ && volumePipeline_ &&
           volumeSet_;
}

bool LinuxPreview::CreateParticles(ex::rhi::Device* device) {
    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kParticlesVS;
    shaderDesc.size = kParticlesVSSize;
    shaderDesc.name = "VSParticle";
    particleVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kParticlesPS;
    shaderDesc.size = kParticlesPSSize;
    shaderDesc.name = "PSAdditive";
    particlePS_ = device->CreateShader(shaderDesc);
    shaderDesc.data = kParticlesPSFire;
    shaderDesc.size = kParticlesPSFireSize;
    shaderDesc.name = "PSFire";
    particleFirePS_ = device->CreateShader(shaderDesc);
    shaderDesc.data = kParticlesPSAlpha;
    shaderDesc.size = kParticlesPSAlphaSize;
    shaderDesc.name = "PSAlpha";
    particleAlphaPS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel, 3},
        {8, ex::rhi::DescriptorType::StorageBuffer, 1,
         ex::rhi::ShaderStage::Vertex},
        {9, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {16, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = particleVS_.get();
    pipelineDesc.pixelShader = particlePS_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 4;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    pipelineDesc.blendEnable = true;
    pipelineDesc.additiveBlend = true;
    particlePipeline_ = device->CreatePipeline(pipelineDesc);
    if (particlePipeline_) {
        particleSet_ = device->CreateDescriptorSet(particlePipeline_.get());
    }
    if (particleSet_) {
        particleSet_->SetBuffer(0, frameConstants_.get(), 0,
                                frameConstants_->Size());
        particleSet_->SetImage(9, volumeDepthTexture_.get());
        particleSet_->SetSampler(16, volumeSampler_.get());
    }

    pipelineDesc.pixelShader = particleFirePS_.get();
    pipelineDesc.blendEnable = true;
    pipelineDesc.additiveBlend = true;
    pipelineDesc.premultipliedBlend = false;
    particleFirePipeline_ = device->CreatePipeline(pipelineDesc);
    if (particleFirePipeline_) {
        particleFireSet_ =
            device->CreateDescriptorSet(particleFirePipeline_.get());
    }
    if (particleFireSet_) {
        particleFireSet_->SetBuffer(0, frameConstants_.get(), 0,
                                    frameConstants_->Size());
        particleFireSet_->SetImage(9, volumeDepthTexture_.get());
        particleFireSet_->SetSampler(16, volumeSampler_.get());
    }

    pipelineDesc.pixelShader = particleAlphaPS_.get();
    pipelineDesc.additiveBlend = false;
    particleAlphaPipeline_ = device->CreatePipeline(pipelineDesc);
    if (particleAlphaPipeline_) {
        particleAlphaSet_ =
            device->CreateDescriptorSet(particleAlphaPipeline_.get());
    }
    if (particleAlphaSet_) {
        particleAlphaSet_->SetBuffer(0, frameConstants_.get(), 0,
                                     frameConstants_->Size());
        particleAlphaSet_->SetImage(9, volumeDepthTexture_.get());
        particleAlphaSet_->SetSampler(16, volumeSampler_.get());
    }
    return particleVS_ && particlePS_ && particlePipeline_ && particleSet_ &&
           particleFirePS_ && particleFirePipeline_ && particleFireSet_ &&
           particleAlphaPS_ && particleAlphaPipeline_ && particleAlphaSet_;
}

bool LinuxPreview::CreateShock(ex::rhi::Device* device) {


    ex::rhi::SamplerDesc samplerDesc{};
    samplerDesc.linear = true;
    samplerDesc.clamp = true;
    shockSampler_ = device->CreateSampler(samplerDesc);

    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kShockVS;
    shaderDesc.size = kShockVSSize;
    shaderDesc.name = "FullscreenVS";
    shockVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kShockPS;
    shaderDesc.size = kShockPSSize;
    shaderDesc.name = "PSShock";
    shockPS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bindings[] = {
        {0, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
        {8, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {9, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {16, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = shockVS_.get();
    pipelineDesc.pixelShader = shockPS_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 4;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    shockPipeline_ = device->CreatePipeline(pipelineDesc);
    if (shockPipeline_) {
        shockSet_ = device->CreateDescriptorSet(shockPipeline_.get());
    }
    if (shockSet_) {
        shockSet_->SetBuffer(0, frameConstants_.get(), 0,
                             frameConstants_->Size());
        shockSet_->SetImage(8, hdrTarget_.get());
        shockSet_->SetImage(9, shockDepthProxy_.get());
        shockSet_->SetSampler(16, shockSampler_.get());
    }
    return shockTarget_ && shockDepthProxy_ && shockSampler_ &&
           shockVS_ && shockPS_ && shockPipeline_ && shockSet_;
}

bool LinuxPreview::CreateComposite(ex::rhi::Device* device) {
    ex::rhi::ShaderDesc shaderDesc{};
    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kCompositeVS;
    shaderDesc.size = kCompositeVSSize;
    shaderDesc.name = "FullscreenVS";
    compositeVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kCompositePS;
    shaderDesc.size = kCompositePSSize;
    shaderDesc.name = "PSComposite";
    compositePS_ = device->CreateShader(shaderDesc);

    ex::rhi::SamplerDesc samplerDesc{};
    samplerDesc.linear = true;
    samplerDesc.clamp = true;
    compositeSampler_ = device->CreateSampler(samplerDesc);

    shaderDesc.stage = ex::rhi::ShaderStage::Vertex;
    shaderDesc.data = kBloomVS;
    shaderDesc.size = kBloomVSSize;
    shaderDesc.name = "FullscreenVS";
    bloomVS_ = device->CreateShader(shaderDesc);
    shaderDesc.stage = ex::rhi::ShaderStage::Pixel;
    shaderDesc.data = kBloomDownPS;
    shaderDesc.size = kBloomDownPSSize;
    shaderDesc.name = "PSDownsample";
    bloomDownPS_ = device->CreateShader(shaderDesc);
    shaderDesc.data = kBloomUpPS;
    shaderDesc.size = kBloomUpPSSize;
    shaderDesc.name = "PSUpsample";
    bloomUpPS_ = device->CreateShader(shaderDesc);

    const ex::rhi::DescriptorBinding bloomBindings[] = {
        {8, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {16, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
        {1, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc bloomPipelineDesc{};
    bloomPipelineDesc.vertexShader = bloomVS_.get();
    bloomPipelineDesc.pixelShader = bloomDownPS_.get();
    bloomPipelineDesc.descriptorBindings = bloomBindings;
    bloomPipelineDesc.descriptorBindingCount = 3;
    bloomPipelineDesc.depthTest = false;
    bloomPipelineDesc.depthWrite = false;
    bloomDownPipeline_ = device->CreatePipeline(bloomPipelineDesc);
    bloomPipelineDesc.pixelShader = bloomUpPS_.get();
    bloomUpPipeline_ = device->CreatePipeline(bloomPipelineDesc);
    if (bloomDownPipeline_) {
        bloomDownSet_ = device->CreateDescriptorSet(bloomDownPipeline_.get());
    }
    if (bloomUpPipeline_) {
        bloomUpSet_ = device->CreateDescriptorSet(bloomUpPipeline_.get());
    }
    if (bloomDownSet_) {
        bloomDownSet_->SetImage(8, shockTarget_.get());
        bloomDownSet_->SetSampler(16, compositeSampler_.get());
        bloomDownSet_->SetBuffer(1, bloomConstants_.get(), 0,
                                 bloomConstants_->Size());
    }
    if (bloomUpSet_) {
        bloomUpSet_->SetImage(8, bloomDownTarget_.get());
        bloomUpSet_->SetSampler(16, compositeSampler_.get());
        bloomUpSet_->SetBuffer(1, bloomConstants_.get(), 0,
                               bloomConstants_->Size());
    }

    const ex::rhi::DescriptorBinding bindings[] = {
        {8, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {9, ex::rhi::DescriptorType::SampledImage, 1,
         ex::rhi::ShaderStage::Pixel},
        {16, ex::rhi::DescriptorType::Sampler, 1,
         ex::rhi::ShaderStage::Pixel},
        {1, ex::rhi::DescriptorType::UniformBuffer, 1,
         ex::rhi::ShaderStage::Pixel},
    };
    ex::rhi::PipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = compositeVS_.get();
    pipelineDesc.pixelShader = compositePS_.get();
    pipelineDesc.descriptorBindings = bindings;
    pipelineDesc.descriptorBindingCount = 4;
    pipelineDesc.depthTest = false;
    pipelineDesc.depthWrite = false;
    compositePipeline_ = device->CreatePipeline(pipelineDesc);
    if (compositePipeline_) {
        compositeSet_ = device->CreateDescriptorSet(compositePipeline_.get());
    }
    if (compositeSet_) {
        compositeSet_->SetImage(8, shockTarget_.get());
        compositeSet_->SetImage(9, bloomUpTarget_.get());
        compositeSet_->SetSampler(16, compositeSampler_.get());
        compositeSet_->SetBuffer(1, bloomConstants_.get(), 0,
                                 bloomConstants_->Size());
    }
    return compositeVS_ && compositePS_ && compositeSampler_ &&
           compositePipeline_ && compositeSet_ && bloomVS_ && bloomDownPS_ &&
           bloomUpPS_ && bloomDownPipeline_ && bloomUpPipeline_ &&
           bloomDownSet_ && bloomUpSet_;
}

void LinuxPreview::SetGpuVolume(ex::rhi::Texture* texture,
                                const ex::sim::VolumeBounds& bounds,
                                float extinction, uint64_t version) {
    if (!texture) {
        return;
    }
    const bool textureChanged = texture != gpuVolumeTexture_;
    gpuVolumeTexture_ = texture;
    gpuVolumeBounds_ = bounds;
    gpuVolumeExtinction_ = extinction;
    gpuVolumeValid_ = true;
    volumeVersion_ = version;
    if (!textureChanged) {
        return;
    }
    if (groundSet_) {
        groundSet_->SetImage(14, texture);
    }
    if (volumeSet_) {
        volumeSet_->SetImage(8, texture);
    }
    if (chunkSet_) {
        chunkSet_->SetImage(15, texture);
    }
    for (SceneDraw& draw : sceneDraws_) {
        if (draw.set) {
            draw.set->SetImage(15, texture);
        }
    }
}

void LinuxPreview::SetGpuLightVolume(ex::rhi::Texture* texture,
                                     const ex::sim::VolumeBounds& bounds) {
    if (!texture) {
        return;
    }
    const bool textureChanged = texture != gpuLightVolume_;
    gpuLightVolume_ = texture;
    gpuLightBounds_ = bounds;
    gpuLightValid_ = true;
    if (textureChanged && volumeSet_) {
        volumeSet_->SetImage(12, texture);
    }
}

void LinuxPreview::SetGpuParticles(ex::rhi::Buffer* additive,
                                   ex::rhi::Buffer* alpha,
                                   ex::rhi::Buffer* fire,
                                   ex::rhi::Buffer* indirect) {
    if (!additive || !alpha || !fire || !indirect) {
        return;
    }
    gpuParticleAdditive_ = additive;
    gpuParticleAlpha_ = alpha;
    gpuParticleFire_ = fire;
    gpuParticleIndirect_ = indirect;
    auto makeSet = [&](ex::rhi::Pipeline* pipeline, ex::rhi::Buffer* buffer) {
        if (!pipeline) {
            return std::unique_ptr<ex::rhi::DescriptorSet>();
        }
        auto set = device_->CreateDescriptorSet(pipeline);
        if (set) {
            set->SetBuffer(0, frameConstants_.get(), 0,
                           frameConstants_->Size());
            set->SetBuffer(8, buffer, 0, buffer->Size());
            set->SetImage(9, volumeDepthTexture_.get());
            set->SetSampler(16, volumeSampler_.get());
        }
        return set;
    };
    particleGpuAdditiveSet_ = makeSet(particlePipeline_.get(), additive);
    particleGpuAlphaSet_ = makeSet(particleAlphaPipeline_.get(), alpha);
    particleGpuFireSet_ = makeSet(particleFirePipeline_.get(), fire);
}

bool LinuxPreview::UploadVolume(const ex::sim::VolumeFrame& frame) {
    if (!device_ || frame.empty() || !frame.halfFloat ||
        frame.bytesPerVoxel != 8) {
        return false;
    }
    // The RHI has no texture update path yet; replacing the 3D image must not
    // race the previous frame that sampled it.
    device_->WaitIdle();
    const uint32_t resolution = uint32_t(frame.resolution);
    ex::rhi::TextureSubresource subresource{};
    subresource.data = frame.rgba.data();
    subresource.width = resolution;
    subresource.height = resolution;
    subresource.rowPitch = resolution * 8;
    subresource.slicePitch = resolution * resolution * 8;
    ex::rhi::TextureDesc desc{};
    desc.format = ex::rhi::Format::R16G16B16A16_FLOAT;
    desc.width = resolution;
    desc.height = resolution;
    desc.depth = resolution;
    desc.usage = ex::rhi::TextureUsage::Sampled;
    desc.subresources = &subresource;
    desc.subresourceCount = 1;
    std::unique_ptr<ex::rhi::Texture> texture = device_->CreateTexture(desc);
    if (!texture) {
        LogError("Could not upload the %u^3 volume texture.", resolution);
        return false;
    }
    volumeTexture_ = std::move(texture);
    gpuVolumeValid_ = false;
    if (groundSet_) {
        groundSet_->SetImage(14, volumeTexture_.get());
    }
    if (volumeSet_) {
        volumeSet_->SetImage(8, volumeTexture_.get());
    }
    if (chunkSet_) {
        chunkSet_->SetImage(15, volumeTexture_.get());
    }
    for (SceneDraw& draw : sceneDraws_) {
        if (draw.set) {
            draw.set->SetImage(15, volumeTexture_.get());
        }
    }
    return true;
}

void LinuxPreview::UploadParticleBatch(
    std::unique_ptr<ex::rhi::Buffer>& buffer, size_t& capacity,
    ex::rhi::DescriptorSet* set, uint32_t& count,
    const std::vector<ex::sim::ParticleInstance>& particles) {
    if (!device_ || !set) {
        count = 0;
        return;
    }
    const size_t particleCount = particles.size();
    if (particleCount > capacity || !buffer) {
        device_->WaitIdle();
        const size_t newCapacity = std::max<size_t>(particleCount, 64);
        ex::rhi::BufferDesc desc{};
        desc.size = newCapacity * sizeof(ex::sim::ParticleInstance);
        desc.usage = ex::rhi::BufferUsage::Storage |
                     ex::rhi::BufferUsage::ShaderRead |
                     ex::rhi::BufferUsage::Dynamic;
        desc.structureStride = sizeof(ex::sim::ParticleInstance);
        desc.initialData =
            particleCount > 0 ? particles.data() : nullptr;
        std::unique_ptr<ex::rhi::Buffer> newBuffer =
            device_->CreateBuffer(desc);
        if (!newBuffer) {
            count = 0;
            return;
        }
        buffer = std::move(newBuffer);
        capacity = newCapacity;
        set->SetBuffer(8, buffer.get(), 0, buffer->Size());
    }
    if (particleCount > 0) {
        void* mapped = buffer->Map();
        if (mapped) {
            std::memcpy(mapped, particles.data(),
                        particleCount * sizeof(ex::sim::ParticleInstance));
            buffer->Unmap();
        }
    }
    count = uint32_t(particleCount);
}

void LinuxPreview::UploadParticles(
    const std::vector<ex::sim::ParticleInstance>& particles) {
    UploadParticleBatch(particleInstanceBuffer_, particleCapacity_,
                        particleSet_.get(), particleCount_, particles);
}

void LinuxPreview::UpdateFrameConstants(const ex::sim::Simulation& simulation,
                                       const ex::gfx::Camera& camera, int width,
                                       int height) {
    using namespace DirectX;
    const ex::sim::BlastModel& blast = simulation.Blast();
    const ex::sim::BlastState& state = blast.State();
    const ex::sim::Environment& environment = blast.GetEnvironment();

    std::memset(frameConstantsData_, 0, sizeof(frameConstantsData_));
    const XMMATRIX view = camera.ViewMatrix();
    const XMMATRIX projection = camera.ProjectionMatrix();
    const XMMATRIX viewProjection = view * projection;
    const XMMATRIX inverseViewProjection =
        XMMatrixInverse(nullptr, viewProjection);
    XMFLOAT4X4 vp{};
    XMFLOAT4X4 inv{};
    XMStoreFloat4x4(&vp, viewProjection);
    XMStoreFloat4x4(&inv, inverseViewProjection);
    std::memcpy(&frameConstantsData_[0], &vp, sizeof(vp));
    std::memcpy(&frameConstantsData_[16], &inv, sizeof(inv));

    XMFLOAT3 cameraRight{};
    XMFLOAT3 cameraUp{};
    camera.Basis(cameraRight, cameraUp);
    const XMFLOAT3 position = camera.Position();
    frameConstantsData_[32] = position.x;
    frameConstantsData_[33] = position.y;
    frameConstantsData_[34] = position.z;
    frameConstantsData_[35] = float(state.time);

    const float azimuth = XMConvertToRadians(settings_.sunAzimuthDeg);
    const float elevation = XMConvertToRadians(settings_.sunElevationDeg);
    const XMFLOAT3 sunDirection(std::cos(elevation) * std::sin(azimuth),
                                std::sin(elevation),
                                std::cos(elevation) * std::cos(azimuth));
    frameConstantsData_[36] = sunDirection.x;
    frameConstantsData_[37] = sunDirection.y;
    frameConstantsData_[38] = sunDirection.z;
    frameConstantsData_[39] = settings_.exposure;
    frameConstantsData_[40] = 1.0f;
    frameConstantsData_[41] = 0.70f;
    frameConstantsData_[42] = 0.44f;
    frameConstantsData_[43] = settings_.bloomStrength;

    XMFLOAT3 volumeMin{0.0f, 0.0f, 0.0f};
    XMFLOAT3 volumeMax{1.0f, 1.0f, 1.0f};
    float extinctionScale = 0.1f;
    int volumeResolution = 1;
    if (gpuVolumeValid_) {
        volumeMin = gpuVolumeBounds_.min;
        volumeMax = gpuVolumeBounds_.max;
        extinctionScale = gpuVolumeExtinction_;
        volumeResolution = gpuVolumeTexture_
                               ? std::max(int(gpuVolumeTexture_->Depth()), 1)
                               : 1;
    } else {
        const ex::sim::VolumeFrame& volume = simulation.Volume().Current();
        const bool volumeValid =
            simulation.Volume().HasData() && !volume.empty();
        if (volumeValid) {
            volumeMin = volume.bounds.min;
            volumeMax = volume.bounds.max;
            extinctionScale = volume.extinctionScale;
            volumeResolution = std::max(volume.resolution, 1);
        }
    }
    frameConstantsData_[44] = volumeMin.x;
    frameConstantsData_[45] = volumeMin.y;
    frameConstantsData_[46] = volumeMin.z;
    frameConstantsData_[47] = extinctionScale;
    frameConstantsData_[48] = volumeMax.x;
    frameConstantsData_[49] = volumeMax.y;
    frameConstantsData_[50] = volumeMax.z;
    frameConstantsData_[51] = float(settings_.raymarchSteps);

    frameConstantsData_[52] = float(width);
    frameConstantsData_[53] = float(height);
    frameConstantsData_[54] = 1.0f / float(std::max(width, 1));
    frameConstantsData_[55] = 1.0f / float(std::max(height, 1));

    const double scorch = std::max(blast.RadiusForThermalFluence(40000.0),
                                   state.fireballRadius * 1.6);
    const double blastRadius = blast.RadiusForOverpressure(34474.0);
    frameConstantsData_[56] = float(scorch);
    frameConstantsData_[57] = float(blastRadius);
    frameConstantsData_[58] = float(state.dustRingRadius);
    frameConstantsData_[59] = float(environment.temperatureK);
    frameConstantsData_[60] = float(state.cloudRadius);
    frameConstantsData_[61] = float(state.cloudCenterY);
    frameConstantsData_[62] = float(state.fireballRadius);
    frameConstantsData_[63] = float(state.fireballTemperatureK);
    frameConstantsData_[64] = float(state.time);
    frameConstantsData_[65] = float(state.time) - lastSimTime_;
    frameConstantsData_[66] = float(frameIndex_);
    frameConstantsData_[67] = float(state.shockRadius);
    lastSimTime_ = float(state.time);
    ++frameIndex_;

    frameConstantsData_[68] = float(environment.WindX());
    frameConstantsData_[69] = 0.0f;
    frameConstantsData_[70] = float(environment.WindZ());
    frameConstantsData_[71] = float(environment.windSpeedMs);
    frameConstantsData_[72] = cameraRight.x;
    frameConstantsData_[73] = cameraRight.y;
    frameConstantsData_[74] = cameraRight.z;
    frameConstantsData_[75] = 0.0f;
    frameConstantsData_[76] = cameraUp.x;
    frameConstantsData_[77] = cameraUp.y;
    frameConstantsData_[78] = cameraUp.z;
    frameConstantsData_[79] = 0.0f;

    const float targetSpacing = std::max(camera.Distance() / 12.0f, 1.0f);
    const float exponent = std::floor(std::log10(targetSpacing));
    const float spacing = std::clamp(std::pow(10.0f, exponent), 1.0f, 5000.0f);
    frameConstantsData_[80] = spacing;
    frameConstantsData_[81] = 1.0f;
    frameConstantsData_[82] = settings_.showGrid ? 0.06f : 0.0f;
    frameConstantsData_[83] = std::max(camera.Distance() * 8.0f, 500.0f);
    frameConstantsData_[84] = camera.NearPlane();
    frameConstantsData_[85] = camera.FarPlane();
    frameConstantsData_[86] = settings_.enableVolume ? 1.0f : 0.0f;
    frameConstantsData_[87] = settings_.particleScale;

    const float fireballLight =
        state.fireballVisible
            ? std::clamp(float((state.fireballTemperatureK - 700.0) / 5000.0),
                         0.0f, 1.4f)
            : 0.0f;
    frameConstantsData_[88] = 0.0f;
    frameConstantsData_[89] = float(state.fireballCenterY);
    frameConstantsData_[90] = 0.0f;
    frameConstantsData_[91] = fireballLight;

    const float voxelSize =
        (volumeMax.x - volumeMin.x) / float(volumeResolution);
    frameConstantsData_[92] = voxelSize;
    frameConstantsData_[93] = 0.0f;
    frameConstantsData_[94] = 1.0f;
    frameConstantsData_[95] = 1.0f;
    if (highFidelity_) {
        // Match the Windows high-fidelity shading: a denser, dimmer medium with
        // the solver's precomputed sun transmittance instead of the procedural
        // light model.
        frameConstantsData_[96] = 0.40f;
        frameConstantsData_[97] = 0.16f;
        frameConstantsData_[98] = 0.0f;
        frameConstantsData_[99] = 1.0f;
    } else {
        frameConstantsData_[96] = 1.0f;
        frameConstantsData_[97] = 1.0f;
        frameConstantsData_[98] = 1.0f;
        frameConstantsData_[99] = 0.0f;
    }

    XMFLOAT3 lightMin = volumeMin;
    XMFLOAT3 lightMax = volumeMax;
    if (gpuLightValid_) {
        lightMin = gpuLightBounds_.min;
        lightMax = gpuLightBounds_.max;
    }
    frameConstantsData_[100] = lightMin.x;
    frameConstantsData_[101] = lightMin.y;
    frameConstantsData_[102] = lightMin.z;
    frameConstantsData_[103] = 0.0f;
    frameConstantsData_[104] = lightMax.x;
    frameConstantsData_[105] = lightMax.y;
    frameConstantsData_[106] = lightMax.z;
    frameConstantsData_[107] = 0.0f;
    frameConstantsData_[108] = 0.0f;
    frameConstantsData_[109] = float(blast.GetCharge().heightOfBurstM);
    frameConstantsData_[110] = 0.0f;
    frameConstantsData_[111] = float(state.shockMach);

    if (frameConstants_) {
        void* mapped = frameConstants_->Map();
        if (mapped) {
            std::memcpy(mapped, frameConstantsData_,
                        sizeof(frameConstantsData_));
            frameConstants_->Unmap();
        }
    }

    bloomConstantsData_[0] = 1.0f / float(std::max(width, 1));
    bloomConstantsData_[1] = 1.0f / float(std::max(height, 1));
    bloomConstantsData_[2] = bloomConstantsData_[0];
    bloomConstantsData_[3] = bloomConstantsData_[1];
    bloomConstantsData_[4] = 0.5f;
    bloomConstantsData_[5] = 1.0f;
    bloomConstantsData_[6] = settings_.exposure;
    bloomConstantsData_[7] = settings_.bloomStrength;
    if (bloomConstants_) {
        void* mapped = bloomConstants_->Map();
        if (mapped) {
            std::memcpy(mapped, bloomConstantsData_,
                        sizeof(bloomConstantsData_));
            bloomConstants_->Unmap();
        }
    }
}

void LinuxPreview::Render(ex::rhi::Device* device, ex::sim::Simulation& simulation,
                          const ex::gfx::Camera& camera, int width, int height,
                          bool vsync) {
    if (!device || !commandList_ || !hdrTarget_ || !depthTarget_) {
        return;
    }
    uint64_t sceneKey = simulation.Scene().GenerationHash();
    uint64_t collapsed = 0;
    for (const ex::scene::SceneObject& object : simulation.Scene().Objects()) {
        if (object.collapsed) {
            ++collapsed;
        }
    }
    sceneKey ^= collapsed * 0x9E3779B97F4A7C15ull;
    if (sceneKey != sceneKey_) {
        if (SetScene(simulation.Scene())) {
            sceneKey_ = sceneKey;
        }
    }

    if (!gpuVolumeValid_ && simulation.Volume().HasData()) {
        const ex::sim::VolumeFrame& volume = simulation.Volume().Current();
        if (volume.version != volumeVersion_ && UploadVolume(volume)) {
            volumeVersion_ = volume.version;
            simulation.Volume().MarkUploaded(volume.version);
        }
    }
    if (!gpuParticleIndirect_) {
        UploadParticleBatch(particleInstanceBuffer_, particleCapacity_,
                            particleSet_.get(), particleCount_,
                            simulation.Particles().AdditiveBatch());
        UploadParticleBatch(particleFireBuffer_, particleFireCapacity_,
                            particleFireSet_.get(), particleFireCount_,
                            simulation.Particles().FireBatch());
        UploadParticleBatch(particleAlphaBuffer_, particleAlphaCapacity_,
                            particleAlphaSet_.get(), particleAlphaCount_,
                            simulation.Particles().AlphaBatch());
    }
    UpdateFrameConstants(simulation, camera, width, height);

    const float clear[4] = {0.02f, 0.02f, 0.03f, 1.0f};
    commandList_->BeginRenderPass(hdrTarget_.get(), depthTarget_.get(), true,
                                  true, clear);
    if (skyPipeline_ && skySet_) {
        commandList_->BindPipeline(skyPipeline_.get());
        commandList_->BindDescriptorSet(skySet_.get());
        commandList_->Draw(3);
    }
    if (settings_.enableGround && groundPipeline_ && groundSet_) {
        commandList_->BindPipeline(groundPipeline_.get());
        commandList_->BindDescriptorSet(groundSet_.get());
        commandList_->Draw(3);
    }
    if (scenePipeline_ && sceneVertexBuffer_ && sceneIndexBuffer_) {
        commandList_->BindPipeline(scenePipeline_.get());
        commandList_->BindVertexBuffer(sceneVertexBuffer_.get(),
                                       sizeof(SceneMeshVertex));
        commandList_->BindIndexBuffer(sceneIndexBuffer_.get());
        for (const SceneDraw& draw : sceneDraws_) {
            if (!draw.set) {
                continue;
            }
            commandList_->BindDescriptorSet(draw.set.get());
            commandList_->DrawIndexedInstanced(draw.indexCount,
                                               draw.instanceCount,
                                               draw.indexOffset);
        }
    }
    if (chunkPipeline_ && chunkSet_ && chunkVertexBuffer_ &&
        chunkIndexBuffer_ && !chunkDraws_.empty()) {
        const auto& objects = simulation.Scene().Objects();
        commandList_->BindPipeline(chunkPipeline_.get());
        commandList_->BindVertexBuffer(chunkVertexBuffer_.get(),
                                       sizeof(ChunkVertex));
        commandList_->BindIndexBuffer(chunkIndexBuffer_.get());
        commandList_->BindDescriptorSet(chunkSet_.get());
        for (const ChunkDraw& draw : chunkDraws_) {
            if (draw.objectIndex < 0 ||
                draw.objectIndex >= int(objects.size()) ||
                !objects[draw.objectIndex].collapsed) {
                continue;
            }
            commandList_->DrawIndexedInstanced(draw.indexCount, 1,
                                               draw.indexOffset);
        }
    }
    commandList_->EndRenderPass();

    // The scene depth is now complete. Full-screen passes sample it, so they
    // use a dummy depth attachment and bind the real depth as a sampled image.
    // EndRenderPass submits the scene pass, so the transition runs in its own
    // compute submission before the next render pass.
    commandList_->BeginCompute();
    commandList_->TransitionTexture(depthTarget_.get(),
                                    ex::rhi::TextureState::DepthRead);
    commandList_->EndCompute();
    if (volumeSet_) {
        volumeSet_->SetImage(9, depthTarget_.get());
    }
    if (particleSet_) {
        particleSet_->SetImage(9, depthTarget_.get());
    }
    if (particleFireSet_) {
        particleFireSet_->SetImage(9, depthTarget_.get());
    }
    if (particleAlphaSet_) {
        particleAlphaSet_->SetImage(9, depthTarget_.get());
    }
    if (particleGpuAdditiveSet_) {
        particleGpuAdditiveSet_->SetImage(9, depthTarget_.get());
    }
    if (particleGpuFireSet_) {
        particleGpuFireSet_->SetImage(9, depthTarget_.get());
    }
    if (particleGpuAlphaSet_) {
        particleGpuAlphaSet_->SetImage(9, depthTarget_.get());
    }
    if (shockSet_) {
        shockSet_->SetImage(9, depthTarget_.get());
    }

    if (settings_.enableVolume && volumePipeline_ && volumeSet_) {
        commandList_->BeginRenderPass(hdrTarget_.get(),
                                      volumeDepthTarget_.get(), false, false,
                                      clear);
        commandList_->BindPipeline(volumePipeline_.get());
        commandList_->BindDescriptorSet(volumeSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }
    if (settings_.enableParticles && gpuParticleIndirect_ &&
        particleGpuAdditiveSet_ && particleGpuAlphaSet_ &&
        particleGpuFireSet_) {
        auto drawIndirect = [&](ex::rhi::Pipeline* pipeline,
                                ex::rhi::DescriptorSet* set,
                                uint64_t offset) {
            if (!pipeline || !set) {
                return;
            }
            commandList_->BeginRenderPass(hdrTarget_.get(),
                                          volumeDepthTarget_.get(), false,
                                          false, clear);
            commandList_->BindPipeline(pipeline);
            commandList_->BindDescriptorSet(set);
            commandList_->DrawInstancedIndirect(gpuParticleIndirect_, offset);
            commandList_->EndRenderPass();
        };
        drawIndirect(particlePipeline_.get(), particleGpuAdditiveSet_.get(), 0);
        drawIndirect(particleFirePipeline_.get(), particleGpuFireSet_.get(), 32);
        drawIndirect(particleAlphaPipeline_.get(), particleGpuAlphaSet_.get(),
                     16);
    } else {
        if (settings_.enableParticles && particlePipeline_ && particleSet_ &&
            particleCount_ > 0) {
            commandList_->BeginRenderPass(hdrTarget_.get(),
                                          volumeDepthTarget_.get(), false,
                                          false, clear);
            commandList_->BindPipeline(particlePipeline_.get());
            commandList_->BindDescriptorSet(particleSet_.get());
            commandList_->DrawInstanced(6, particleCount_);
            commandList_->EndRenderPass();
        }
        if (settings_.enableParticles && particleFirePipeline_ &&
            particleFireSet_ && particleFireCount_ > 0) {
            commandList_->BeginRenderPass(hdrTarget_.get(),
                                          volumeDepthTarget_.get(), false,
                                          false, clear);
            commandList_->BindPipeline(particleFirePipeline_.get());
            commandList_->BindDescriptorSet(particleFireSet_.get());
            commandList_->DrawInstanced(6, particleFireCount_);
            commandList_->EndRenderPass();
        }
        if (settings_.enableParticles && particleAlphaPipeline_ &&
            particleAlphaSet_ && particleAlphaCount_ > 0) {
            commandList_->BeginRenderPass(hdrTarget_.get(),
                                          volumeDepthTarget_.get(), false,
                                          false, clear);
            commandList_->BindPipeline(particleAlphaPipeline_.get());
            commandList_->BindDescriptorSet(particleAlphaSet_.get());
            commandList_->DrawInstanced(6, particleAlphaCount_);
            commandList_->EndRenderPass();
        }
    }
    if (shockPipeline_ && shockSet_) {
        commandList_->BeginRenderPass(shockTarget_.get(),
                                      volumeDepthTarget_.get(), true, false,
                                      clear);
        commandList_->BindPipeline(shockPipeline_.get());
        commandList_->BindDescriptorSet(shockSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }

    if (settings_.enableBloom && bloomDownPipeline_ && bloomDownSet_) {
        commandList_->BeginRenderPass(bloomDownTarget_.get(),
                                      volumeDepthTarget_.get(), true, false,
                                      clear);
        commandList_->BindPipeline(bloomDownPipeline_.get());
        commandList_->BindDescriptorSet(bloomDownSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }
    if (settings_.enableBloom && bloomUpPipeline_ && bloomUpSet_) {
        commandList_->BeginRenderPass(bloomUpTarget_.get(),
                                      volumeDepthTarget_.get(), true, false,
                                      clear);
        commandList_->BindPipeline(bloomUpPipeline_.get());
        commandList_->BindDescriptorSet(bloomUpSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }

    device->BeginFrame(clear[0], clear[1], clear[2], clear[3]);
    if (compositePipeline_ && compositeSet_) {
        device->BindPipeline(compositePipeline_.get());
        device->BindDescriptorSet(compositeSet_.get());
        device->Draw(3);
    }
    if (overlay_) {
        overlay_();
    }
    device->EndFrame(vsync);
}

} // namespace ex::app
