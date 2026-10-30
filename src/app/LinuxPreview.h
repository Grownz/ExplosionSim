#pragma once

#include "gfx/rhi/Rhi.h"
#include "scene/SceneTypes.h"
#include "sim/ParticleSystem.h"
#include "sim/VolumeBaker.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ex::gfx {
class Camera;
}
namespace ex::sim {
class Simulation;
}

namespace ex::app {

// Reduced-order preview used by the native Linux shell while the Windows
// renderer is ported pass by pass. It owns the HDR/depth targets, the sky,
// ground and procedural scene pipelines, the volume/particle/shock passes and
// the composite pass.
class LinuxPreview {
public:
    struct Settings {
        int raymarchSteps = 128;
        float exposure = 0.8f;
        float bloomStrength = 0.10f;
        bool enableVolume = true;
        bool enableParticles = true;
        bool enableGround = true;
        bool enableBloom = true;
        bool showGrid = true;
        float sunAzimuthDeg = 302.0f;
        float sunElevationDeg = 13.0f;
        float particleScale = 1.0f;
    };

    bool Initialize(ex::rhi::Device* device, int width, int height);
    void Shutdown();
    void SetSettings(const Settings& settings) { settings_ = settings; }
    void Render(ex::rhi::Device* device, ex::sim::Simulation& simulation,
                const ex::gfx::Camera& camera, int width, int height,
                bool vsync);

private:
    struct MeshRange {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
    };

    struct SceneDraw {
        std::unique_ptr<ex::rhi::Buffer> instanceBuffer;
        std::unique_ptr<ex::rhi::DescriptorSet> set;
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        uint32_t instanceCount = 0;
    };

    bool CreateTargets(ex::rhi::Device* device, int width, int height);
    bool CreateSky(ex::rhi::Device* device);
    bool CreateGround(ex::rhi::Device* device);
    bool CreateScene(ex::rhi::Device* device);
    bool CreateVolume(ex::rhi::Device* device);
    bool CreateParticles(ex::rhi::Device* device);
    bool CreateShock(ex::rhi::Device* device);
    bool CreateComposite(ex::rhi::Device* device);
    bool SetScene(const ex::scene::Scene& scene);
    bool UploadVolume(const ex::sim::VolumeFrame& frame);
    void UploadParticles(
        const std::vector<ex::sim::ParticleInstance>& particles);
    void UploadParticleBatch(
        std::unique_ptr<ex::rhi::Buffer>& buffer, size_t& capacity,
        ex::rhi::DescriptorSet* set, uint32_t& count,
        const std::vector<ex::sim::ParticleInstance>& particles);
    void UpdateFrameConstants(const ex::sim::Simulation& simulation,
                              const ex::gfx::Camera& camera, int width,
                              int height);

    std::unique_ptr<ex::rhi::CommandList> commandList_;
    ex::rhi::Device* device_ = nullptr;
    std::unique_ptr<ex::rhi::Texture> hdrTarget_;
    std::unique_ptr<ex::rhi::Texture> depthTarget_;
    std::unique_ptr<ex::rhi::Buffer> frameConstants_;

    // Sky.
    std::unique_ptr<ex::rhi::Shader> skyVS_;
    std::unique_ptr<ex::rhi::Shader> skyPS_;
    std::unique_ptr<ex::rhi::Pipeline> skyPipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> skySet_;

    // Ground and shared shadow/volume inputs.
    std::unique_ptr<ex::rhi::Buffer> shadowConstants_;
    std::unique_ptr<ex::rhi::Texture> shadowTextures_[4];
    std::unique_ptr<ex::rhi::Texture> volumeTexture_;
    std::unique_ptr<ex::rhi::Sampler> shadowSampler_;
    std::unique_ptr<ex::rhi::Sampler> volumeSampler_;
    std::unique_ptr<ex::rhi::Shader> groundVS_;
    std::unique_ptr<ex::rhi::Shader> groundPS_;
    std::unique_ptr<ex::rhi::Pipeline> groundPipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> groundSet_;

    // Procedural scene.
    std::unique_ptr<ex::rhi::Buffer> sceneVertexBuffer_;
    std::unique_ptr<ex::rhi::Buffer> sceneIndexBuffer_;
    std::unique_ptr<ex::rhi::Buffer> sceneCascadeBuffer_;
    std::unique_ptr<ex::rhi::Texture> surfaceTexture_;
    std::unique_ptr<ex::rhi::Shader> sceneVS_;
    std::unique_ptr<ex::rhi::Shader> scenePS_;
    std::unique_ptr<ex::rhi::Pipeline> scenePipeline_;
    std::vector<SceneDraw> sceneDraws_;
    MeshRange buildingRange_;
    MeshRange vehicleRange_;
    MeshRange treeRange_;
    uint64_t sceneKey_ = 0;

    // Volumetric smoke.
    std::unique_ptr<ex::rhi::Texture> volumeNoiseTexture_;
    std::unique_ptr<ex::rhi::Texture> volumeLightTexture_;
    std::unique_ptr<ex::rhi::Texture> volumeAuxTexture_;
    std::unique_ptr<ex::rhi::Texture> volumeDepthTexture_;
    std::unique_ptr<ex::rhi::Sampler> volumeNoiseSampler_;
    std::unique_ptr<ex::rhi::Shader> volumeVS_;
    std::unique_ptr<ex::rhi::Shader> volumePS_;
    std::unique_ptr<ex::rhi::Pipeline> volumePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> volumeSet_;
    uint64_t volumeVersion_ = 0;

    // Additive particles.
    std::unique_ptr<ex::rhi::Buffer> particleInstanceBuffer_;
    std::unique_ptr<ex::rhi::Shader> particleVS_;
    std::unique_ptr<ex::rhi::Shader> particlePS_;
    std::unique_ptr<ex::rhi::Pipeline> particlePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> particleSet_;
    uint32_t particleCount_ = 0;
    size_t particleCapacity_ = 0;
    std::unique_ptr<ex::rhi::Buffer> particleFireBuffer_;
    std::unique_ptr<ex::rhi::Shader> particleFirePS_;
    std::unique_ptr<ex::rhi::Pipeline> particleFirePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> particleFireSet_;
    uint32_t particleFireCount_ = 0;
    size_t particleFireCapacity_ = 0;
    std::unique_ptr<ex::rhi::Buffer> particleAlphaBuffer_;
    std::unique_ptr<ex::rhi::Shader> particleAlphaPS_;
    std::unique_ptr<ex::rhi::Pipeline> particleAlphaPipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> particleAlphaSet_;
    uint32_t particleAlphaCount_ = 0;
    size_t particleAlphaCapacity_ = 0;

    // Shock-front refraction.
    std::unique_ptr<ex::rhi::Texture> shockTarget_;
    std::unique_ptr<ex::rhi::Texture> shockDepthProxy_;
    std::unique_ptr<ex::rhi::Sampler> shockSampler_;
    std::unique_ptr<ex::rhi::Shader> shockVS_;
    std::unique_ptr<ex::rhi::Shader> shockPS_;
    std::unique_ptr<ex::rhi::Pipeline> shockPipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> shockSet_;

    // Composite.
    std::unique_ptr<ex::rhi::Texture> bloomTexture_;
    std::unique_ptr<ex::rhi::Texture> bloomDownTarget_;
    std::unique_ptr<ex::rhi::Texture> bloomUpTarget_;
    std::unique_ptr<ex::rhi::Sampler> compositeSampler_;
    std::unique_ptr<ex::rhi::Buffer> bloomConstants_;
    std::unique_ptr<ex::rhi::Shader> bloomVS_;
    std::unique_ptr<ex::rhi::Shader> bloomDownPS_;
    std::unique_ptr<ex::rhi::Shader> bloomUpPS_;
    std::unique_ptr<ex::rhi::Pipeline> bloomDownPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> bloomUpPipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> bloomDownSet_;
    std::unique_ptr<ex::rhi::DescriptorSet> bloomUpSet_;
    std::unique_ptr<ex::rhi::Shader> compositeVS_;
    std::unique_ptr<ex::rhi::Shader> compositePS_;
    std::unique_ptr<ex::rhi::Pipeline> compositePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> compositeSet_;

    float frameConstantsData_[112]{};
    float bloomConstantsData_[8]{};
    Settings settings_{};
    float lastSimTime_ = 0.0f;
    uint32_t frameIndex_ = 0;
};

} // namespace ex::app
