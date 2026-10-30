#include "app/LinuxPreview.h"

#include "core/Log.h"
#include "gfx/SceneMeshLibrary.h"
#include "scene/SceneTypes.h"
#include "sim/BlastModel.h"

#include "BloomShaders.h"
#include "CompositeShaders.h"
#include "GroundShaders.h"
#include "ParticleShaders.h"
#include "SceneShaders.h"
#include "ShockShaders.h"
#include "SkyShaders.h"
#include "VolumeShaders.h"

#include <DirectXMath.h>

#include <algorithm>
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

} // namespace

bool LinuxPreview::Initialize(ex::rhi::Device* device, int width, int height) {
    if (!device) {
        return false;
    }
    if (!CreateTargets(device, width, height) || !CreateSky(device) ||
        !CreateGround(device) || !CreateScene(device) ||
        !CreateVolume(device) || !CreateParticles(device) ||
        !CreateShock(device) || !CreateComposite(device)) {
        return false;
    }
    UpdateFrameConstants(width, height, 0.0f);
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
    shockUniformBuffer_.reset();
    shockSampler_.reset();
    shockDepthProxy_.reset();
    shockTarget_.reset();
    particleSet_.reset();
    particlePipeline_.reset();
    particlePS_.reset();
    particleVS_.reset();
    particleInstanceBuffer_.reset();
    volumeSet_.reset();
    volumePipeline_.reset();
    volumePS_.reset();
    volumeVS_.reset();
    volumeUniformBuffer_.reset();
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

    std::vector<uint8_t> volumeDepthPixels(
        size_t(std::max(width, 1)) * size_t(std::max(height, 1)) * 4, 230);
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

    const uint8_t surfacePixels[16] = {
        128, 128, 128, 255, 128, 128, 128, 255,
        128, 128, 128, 255, 128, 128, 128, 255,
    };
    ex::rhi::TextureSubresource surfaceSubresource{};
    surfaceSubresource.data = surfacePixels;
    surfaceSubresource.width = 2;
    surfaceSubresource.height = 2;
    surfaceSubresource.rowPitch = 8;
    ex::rhi::TextureDesc surfaceDesc{};
    surfaceDesc.format = ex::rhi::Format::R8G8B8A8_UNORM;
    surfaceDesc.width = 2;
    surfaceDesc.height = 2;
    surfaceDesc.usage = ex::rhi::TextureUsage::Sampled;
    surfaceDesc.subresources = &surfaceSubresource;
    surfaceDesc.subresourceCount = 1;
    surfaceTexture_ = device->CreateTexture(surfaceDesc);

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
    bloomConstantsData_[6] = 1.0f;
    bloomConstantsData_[7] = 1.0f;
    ex::rhi::BufferDesc bloomDescBuffer{};
    bloomDescBuffer.size = sizeof(bloomConstantsData_);
    bloomDescBuffer.usage = ex::rhi::BufferUsage::Uniform |
                            ex::rhi::BufferUsage::Dynamic;
    bloomDescBuffer.initialData = bloomConstantsData_;
    bloomConstants_ = device->CreateBuffer(bloomDescBuffer);

    commandList_ = device->CreateCommandList();
    bool ok = hdrTarget_ && depthTarget_ && frameConstants_ &&
              shadowConstants_ && volumeTexture_ && volumeAuxTexture_ &&
              volumeNoiseTexture_ && volumeLightTexture_ &&
              volumeDepthTexture_ && volumeNoiseSampler_ &&
              shadowSampler_ && volumeSampler_ && surfaceTexture_ &&
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
        groundSet_->SetImage(9, depthTarget_.get());
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
    const MeshRange buildingRange = appendMesh(buildingMesh);
    const MeshRange vehicleRange = appendMesh(vehicleMesh);
    const MeshRange treeRange = appendMesh(treeMesh);

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

    ex::sim::BlastModel blast;
    blast.Configure({}, {});
    ex::scene::SceneSettings settings{};
    settings.seed = 42;
    settings.density = 0.6f;
    settings.quality = 1;
    settings.voronoiChunks = false;
    ex::scene::Scene scene;
    scene.Generate(settings, blast);

    std::vector<SceneInstance> buildingInstances;
    std::vector<SceneInstance> vehicleInstances;
    std::vector<SceneInstance> treeInstances;
    float maxVehicleSize = 0.0f;
    float maxTreeSize = 0.0f;
    for (const ex::scene::SceneObject& object : scene.Objects()) {
        SceneInstance item{};
        DirectX::XMMATRIX world;
        if (object.type == ex::scene::SceneObjectType::Tree) {
            world = DirectX::XMMatrixScaling(object.size.x * 2.0f, object.size.y,
                                             object.size.x * 2.0f) *
                    DirectX::XMMatrixTranslation(object.position.x, 0.0f,
                                                 object.position.z);
            item.params[3] = 2.0f;
            item.textureScale[3] = 2.0f;
            maxTreeSize = std::max(maxTreeSize, object.size.y);
        } else if (object.type == ex::scene::SceneObjectType::Vehicle) {
            const float uniform =
                object.size.z > 1e-4f ? object.size.z / 2.8f : 1.0f;
            world = DirectX::XMMatrixScaling(uniform, uniform, uniform) *
                    DirectX::XMMatrixTranslation(object.position.x, 0.0f,
                                                 object.position.z);
            item.params[3] = 1.0f;
            item.textureScale[3] = 0.0f;
            maxVehicleSize = std::max(maxVehicleSize, object.size.z);
        } else {
            world = DirectX::XMMatrixScaling(object.size.x, object.size.y,
                                             object.size.z) *
                    DirectX::XMMatrixTranslation(object.position.x,
                                                 object.position.y,
                                                 object.position.z);
            item.params[3] = 0.0f;
            item.textureScale[3] = 0.0f;
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

    auto makeInstanceBuffer = [&](const std::vector<SceneInstance>& data) {
        ex::rhi::BufferDesc desc{};
        desc.size = std::max<size_t>(1, data.size()) * sizeof(SceneInstance);
        desc.usage = ex::rhi::BufferUsage::Storage |
                     ex::rhi::BufferUsage::ShaderRead |
                     ex::rhi::BufferUsage::Dynamic;
        desc.structureStride = sizeof(SceneInstance);
        desc.initialData = data.empty() ? nullptr : data.data();
        return device->CreateBuffer(desc);
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
            device->CreateDescriptorSet(scenePipeline_.get());
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
        set->SetSampler(19, volumeSampler_.get());
        set->SetSampler(20, volumeSampler_.get());

        SceneDraw draw;
        draw.instanceBuffer = std::move(instanceBuffer);
        draw.set = std::move(set);
        draw.indexOffset = range.indexOffset;
        draw.indexCount = range.indexCount;
        draw.instanceCount = uint32_t(data.size());
        sceneDraws_.push_back(std::move(draw));
    };
    addDraw(buildingRange, buildingInstances);
    addDraw(vehicleRange, vehicleInstances);
    addDraw(treeRange, treeInstances);
    LogInfo("Preview instances: buildings=%zu vehicles=%zu trees=%zu "
            "maxVehicleZ=%.2f maxTreeY=%.2f",
            buildingInstances.size(), vehicleInstances.size(),
            treeInstances.size(), maxVehicleSize, maxTreeSize);

    return sceneVertexBuffer_ && sceneIndexBuffer_ && sceneVS_ && scenePS_ &&
           scenePipeline_ && !sceneDraws_.empty();
}

bool LinuxPreview::CreateVolume(ex::rhi::Device* device) {
    float constants[112] = {};
    constants[0] = 1.0f;
    constants[5] = 1.0f;
    constants[10] = 1.0f;
    constants[15] = 1.0f;
    constants[16] = 1.0f;
    constants[21] = 1.0f;
    constants[26] = 1.0f;
    constants[31] = 1.0f;
    constants[32] = 0.0f;
    constants[33] = 55.0f;
    constants[34] = -150.0f;
    constants[36] = 0.3f;
    constants[37] = 0.8f;
    constants[38] = 0.5f;
    constants[39] = 1.0f;
    constants[40] = 1.0f;
    constants[41] = 0.7f;
    constants[42] = 0.44f;
    constants[43] = 1.0f;
    constants[44] = -15.0f;
    constants[45] = 0.0f;
    constants[46] = -15.0f;
    constants[47] = 0.05f;
    constants[48] = 15.0f;
    constants[49] = 25.0f;
    constants[50] = 15.0f;
    constants[51] = 32.0f;
    constants[60] = 0.0f;
    constants[61] = 0.0f;
    constants[62] = 8.0f;
    constants[63] = 0.0f;
    constants[64] = 0.0f;
    constants[65] = 1.0f / 60.0f;
    constants[66] = 0.0f;
    constants[67] = 0.0f;
    constants[88] = 0.0f;
    constants[89] = 10.0f;
    constants[90] = 0.0f;
    constants[91] = 1.0f;
    constants[92] = 0.5f;
    constants[93] = 0.0f;
    constants[94] = 0.5f;
    constants[95] = 0.5f;
    constants[96] = 1.0f;
    constants[97] = 0.2f;
    constants[98] = 0.2f;
    constants[99] = 0.0f;

    ex::rhi::BufferDesc uniformDesc{};
    uniformDesc.size = sizeof(constants);
    uniformDesc.usage = ex::rhi::BufferUsage::Uniform |
                        ex::rhi::BufferUsage::Dynamic;
    uniformDesc.initialData = constants;
    volumeUniformBuffer_ = device->CreateBuffer(uniformDesc);

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
        volumeSet_->SetBuffer(0, volumeUniformBuffer_.get(), 0,
                              volumeUniformBuffer_->Size());
        volumeSet_->SetImage(8, volumeTexture_.get());
        volumeSet_->SetImage(9, volumeDepthTexture_.get());
        volumeSet_->SetImage(10, volumeNoiseTexture_.get());
        volumeSet_->SetImage(11, volumeAuxTexture_.get());
        volumeSet_->SetImage(12, volumeLightTexture_.get());
        volumeSet_->SetSampler(16, volumeSampler_.get());
        volumeSet_->SetSampler(17, volumeNoiseSampler_.get());
    }
    return volumeUniformBuffer_ && volumeVS_ && volumePS_ && volumePipeline_ &&
           volumeSet_;
}

bool LinuxPreview::CreateParticles(ex::rhi::Device* device) {
    struct ParticleInstance {
        float position[3];
        float size;
        float color[3];
        float opacity;
        float rotation;
        float variation;
        float padding[2];
    };
    ParticleInstance instance{};
    instance.position[1] = 10.0f;
    instance.position[2] = 0.0f;
    instance.size = 6.0f;
    instance.color[0] = 1.0f;
    instance.color[1] = 0.6f;
    instance.color[2] = 0.2f;
    instance.opacity = 1.0f;
    instance.padding[0] = 1.0f;

    ex::rhi::BufferDesc instanceDesc{};
    instanceDesc.size = sizeof(instance);
    instanceDesc.usage = ex::rhi::BufferUsage::Storage |
                         ex::rhi::BufferUsage::ShaderRead |
                         ex::rhi::BufferUsage::Dynamic;
    instanceDesc.structureStride = sizeof(ParticleInstance);
    instanceDesc.initialData = &instance;
    particleInstanceBuffer_ = device->CreateBuffer(instanceDesc);

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
        particleSet_->SetBuffer(8, particleInstanceBuffer_.get(), 0,
                                particleInstanceBuffer_->Size());
        particleSet_->SetImage(9, depthTarget_.get());
        particleSet_->SetSampler(16, volumeSampler_.get());
    }
    return particleInstanceBuffer_ && particleVS_ && particlePS_ &&
           particlePipeline_ && particleSet_;
}

bool LinuxPreview::CreateShock(ex::rhi::Device* device) {
    float constants[112] = {};
    constants[0] = 1.0f;
    constants[5] = 1.0f;
    constants[10] = 1.0f;
    constants[15] = 1.0f;
    constants[16] = 1.0f;
    constants[21] = 1.0f;
    constants[26] = 1.0f;
    constants[31] = 1.0f;
    constants[32] = 0.0f;
    constants[33] = 55.0f;
    constants[34] = -150.0f;
    constants[36] = 0.3f;
    constants[37] = 0.8f;
    constants[38] = 0.5f;
    constants[39] = 1.0f;
    constants[40] = 1.0f;
    constants[41] = 0.7f;
    constants[42] = 0.44f;
    constants[43] = 1.0f;
    constants[52] = 1600.0f;
    constants[53] = 900.0f;
    constants[54] = 1.0f / 1600.0f;
    constants[55] = 1.0f / 900.0f;
    constants[64] = 0.0f;
    constants[65] = 1.0f / 60.0f;
    constants[66] = 0.0f;
    constants[67] = 60.0f;
    constants[108] = 0.0f;
    constants[109] = 10.0f;
    constants[110] = 0.0f;
    constants[111] = 2.0f;

    ex::rhi::BufferDesc uniformDesc{};
    uniformDesc.size = sizeof(constants);
    uniformDesc.usage = ex::rhi::BufferUsage::Uniform |
                        ex::rhi::BufferUsage::Dynamic;
    uniformDesc.initialData = constants;
    shockUniformBuffer_ = device->CreateBuffer(uniformDesc);

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
        shockSet_->SetBuffer(0, shockUniformBuffer_.get(), 0,
                             shockUniformBuffer_->Size());
        shockSet_->SetImage(8, hdrTarget_.get());
        shockSet_->SetImage(9, shockDepthProxy_.get());
        shockSet_->SetSampler(16, shockSampler_.get());
    }
    return shockTarget_ && shockDepthProxy_ && shockSampler_ &&
           shockUniformBuffer_ && shockVS_ && shockPS_ && shockPipeline_ &&
           shockSet_;
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

void LinuxPreview::UpdateFrameConstants(int width, int height, float time) {
    using namespace DirectX;
    std::memset(frameConstantsData_, 0, sizeof(frameConstantsData_));
    const float aspect = height > 0 ? float(width) / float(height) : 1.0f;
    const XMMATRIX view = XMMatrixLookAtLH(
        XMVectorSet(0.0f, 55.0f, -150.0f, 0.0f),
        XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(50.0f), aspect, 0.5f, 20000.0f);
    const XMMATRIX viewProjection = view * projection;
    const XMMATRIX inverseViewProjection =
        XMMatrixInverse(nullptr, viewProjection);
    XMFLOAT4X4 vp{};
    XMFLOAT4X4 inv{};
    XMStoreFloat4x4(&vp, viewProjection);
    XMStoreFloat4x4(&inv, inverseViewProjection);
    std::memcpy(&frameConstantsData_[0], &vp, sizeof(vp));
    std::memcpy(&frameConstantsData_[16], &inv, sizeof(inv));

    frameConstantsData_[32] = 0.0f;
    frameConstantsData_[33] = 55.0f;
    frameConstantsData_[34] = -150.0f;
    frameConstantsData_[35] = time;
    frameConstantsData_[36] = 0.3f;
    frameConstantsData_[37] = 0.8f;
    frameConstantsData_[38] = 0.5f;
    frameConstantsData_[39] = 1.0f;
    frameConstantsData_[40] = 1.0f;
    frameConstantsData_[41] = 0.7f;
    frameConstantsData_[42] = 0.44f;
    frameConstantsData_[43] = 1.0f;
    frameConstantsData_[44] = 0.0f;
    frameConstantsData_[45] = 0.0f;
    frameConstantsData_[46] = 0.0f;
    frameConstantsData_[47] = 0.1f;
    frameConstantsData_[48] = 1.0f;
    frameConstantsData_[49] = 1.0f;
    frameConstantsData_[50] = 1.0f;
    frameConstantsData_[51] = 32.0f;
    frameConstantsData_[52] = float(width);
    frameConstantsData_[53] = float(height);
    frameConstantsData_[54] = 1.0f / float(std::max(width, 1));
    frameConstantsData_[55] = 1.0f / float(std::max(height, 1));
    frameConstantsData_[56] = 0.0f;
    frameConstantsData_[57] = 0.0f;
    frameConstantsData_[58] = 0.0f;
    frameConstantsData_[59] = 0.2f;
    frameConstantsData_[60] = 0.0f;
    frameConstantsData_[61] = 0.0f;
    frameConstantsData_[62] = 0.5f;
    frameConstantsData_[63] = 0.0f;
    frameConstantsData_[64] = time;
    frameConstantsData_[65] = 1.0f / 60.0f;
    frameConstantsData_[66] = 0.0f;
    frameConstantsData_[67] = 0.0f;
    frameConstantsData_[68] = 1.0f;
    frameConstantsData_[69] = 0.0f;
    frameConstantsData_[70] = 0.0f;
    frameConstantsData_[71] = 1.0f;
    frameConstantsData_[72] = 1.0f;
    frameConstantsData_[77] = 1.0f;
    frameConstantsData_[80] = 10.0f;
    frameConstantsData_[81] = 1.0f;
    frameConstantsData_[82] = 0.3f;
    frameConstantsData_[83] = 100.0f;
    frameConstantsData_[84] = 0.1f;
    frameConstantsData_[85] = 1000.0f;
    frameConstantsData_[86] = 0.0f;
    frameConstantsData_[87] = 1.0f;
    frameConstantsData_[88] = 0.0f;
    frameConstantsData_[89] = 0.0f;
    frameConstantsData_[90] = 0.0f;
    frameConstantsData_[91] = 0.0f;

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
    if (shockUniformBuffer_) {
        void* mapped = shockUniformBuffer_->Map();
        if (mapped) {
            float* values = static_cast<float*>(mapped);
            values[52] = float(width);
            values[53] = float(height);
            values[54] = 1.0f / float(std::max(width, 1));
            values[55] = 1.0f / float(std::max(height, 1));
            values[64] = time;
            values[65] = 1.0f / 60.0f;
            shockUniformBuffer_->Unmap();
        }
    }
    if (bloomConstants_) {
        void* mapped = bloomConstants_->Map();
        if (mapped) {
            std::memcpy(mapped, bloomConstantsData_,
                        sizeof(bloomConstantsData_));
            bloomConstants_->Unmap();
        }
    }
}

void LinuxPreview::Render(ex::rhi::Device* device, int width, int height,
                          float time, bool vsync) {
    if (!device || !commandList_ || !hdrTarget_ || !depthTarget_) {
        return;
    }
    UpdateFrameConstants(width, height, time);

    const float clear[4] = {0.02f, 0.02f, 0.03f, 1.0f};
    commandList_->BeginRenderPass(hdrTarget_.get(), depthTarget_.get(), true,
                                  true, clear);
    if (skyPipeline_ && skySet_) {
        commandList_->BindPipeline(skyPipeline_.get());
        commandList_->BindDescriptorSet(skySet_.get());
        commandList_->Draw(3);
    }
    if (groundPipeline_ && groundSet_) {
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
    commandList_->EndRenderPass();

    if (volumePipeline_ && volumeSet_) {
        commandList_->BeginRenderPass(hdrTarget_.get(), depthTarget_.get(),
                                      false, false, clear);
        commandList_->BindPipeline(volumePipeline_.get());
        commandList_->BindDescriptorSet(volumeSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }
    if (particlePipeline_ && particleSet_) {
        commandList_->BeginRenderPass(hdrTarget_.get(), depthTarget_.get(),
                                      false, false, clear);
        commandList_->BindPipeline(particlePipeline_.get());
        commandList_->BindDescriptorSet(particleSet_.get());
        commandList_->DrawInstanced(6, 1);
        commandList_->EndRenderPass();
    }
    if (shockPipeline_ && shockSet_) {
        commandList_->BeginRenderPass(shockTarget_.get(), depthTarget_.get(),
                                      true, false, clear);
        commandList_->BindPipeline(shockPipeline_.get());
        commandList_->BindDescriptorSet(shockSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }

    if (bloomDownPipeline_ && bloomDownSet_) {
        commandList_->BeginRenderPass(bloomDownTarget_.get(),
                                      depthTarget_.get(), true, false, clear);
        commandList_->BindPipeline(bloomDownPipeline_.get());
        commandList_->BindDescriptorSet(bloomDownSet_.get());
        commandList_->Draw(3);
        commandList_->EndRenderPass();
    }
    if (bloomUpPipeline_ && bloomUpSet_) {
        commandList_->BeginRenderPass(bloomUpTarget_.get(),
                                      depthTarget_.get(), true, false, clear);
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
    device->EndFrame(vsync);
}

} // namespace ex::app
