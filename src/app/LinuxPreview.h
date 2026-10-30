#pragma once

#include "gfx/rhi/Rhi.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ex::app {

// Reduced-order preview used by the native Linux shell while the Windows
// renderer is ported pass by pass. It owns the HDR/depth targets, the sky,
// ground and procedural scene pipelines, and the composite pass.
class LinuxPreview {
public:
    bool Initialize(ex::rhi::Device* device, int width, int height);
    void Shutdown();
    void Render(ex::rhi::Device* device, int width, int height, float time,
                bool vsync);

private:
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
    void UpdateFrameConstants(int width, int height, float time);

    std::unique_ptr<ex::rhi::CommandList> commandList_;
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

    // Volumetric smoke.
    std::unique_ptr<ex::rhi::Texture> volumeNoiseTexture_;
    std::unique_ptr<ex::rhi::Texture> volumeLightTexture_;
    std::unique_ptr<ex::rhi::Texture> volumeAuxTexture_;
    std::unique_ptr<ex::rhi::Texture> volumeDepthTexture_;
    std::unique_ptr<ex::rhi::Sampler> volumeNoiseSampler_;
    std::unique_ptr<ex::rhi::Buffer> volumeUniformBuffer_;
    std::unique_ptr<ex::rhi::Shader> volumeVS_;
    std::unique_ptr<ex::rhi::Shader> volumePS_;
    std::unique_ptr<ex::rhi::Pipeline> volumePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> volumeSet_;

    // Additive particles.
    std::unique_ptr<ex::rhi::Buffer> particleInstanceBuffer_;
    std::unique_ptr<ex::rhi::Shader> particleVS_;
    std::unique_ptr<ex::rhi::Shader> particlePS_;
    std::unique_ptr<ex::rhi::Pipeline> particlePipeline_;
    std::unique_ptr<ex::rhi::DescriptorSet> particleSet_;

    // Shock-front refraction.
    std::unique_ptr<ex::rhi::Texture> shockTarget_;
    std::unique_ptr<ex::rhi::Texture> shockDepthProxy_;
    std::unique_ptr<ex::rhi::Sampler> shockSampler_;
    std::unique_ptr<ex::rhi::Buffer> shockUniformBuffer_;
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
};

} // namespace ex::app
