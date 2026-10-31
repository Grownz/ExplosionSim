#pragma once

#include "gfx/EulerSolverTypes.h"
#include "sim/FlowBodies.h"
#include "sim/Obstacles.h"
#include "gfx/rhi/Rhi.h"
#include "sim/VolumeBaker.h"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ex::gfx {

// RHI port of the high-fidelity compressible Euler solver. It mirrors the
// D3D11 GpuEulerSolver: HLLC fluxes with MUSCL/minmod reconstruction,
// dimensional splitting, buoyancy/soot cooling fused into the last sweep, a
// reduction pass for the CFL speed and visible field bounds, the render-volume
// conversion and the sun-transmittance light volume. State textures ping-pong
// between two RGBA32F/RG32F pairs and stay in the GENERAL layout so sampled and
// storage descriptors can alias them without per-pass transitions.
class RhiEulerSolver {
public:
    struct ShaderSource {
        const void* data = nullptr;
        size_t size = 0;
    };

    struct Shaders {
        ShaderSource init;
        ShaderSource sweepX;
        ShaderSource sweepY;
        ShaderSource sweepZ;
        ShaderSource sweepXSource;
        ShaderSource sweepYSource;
        ShaderSource sweepZSource;
        ShaderSource reduce;
        ShaderSource render;
        ShaderSource light;
        ShaderSource rescale;
        ShaderSource bodyFlow;
    };

    bool Initialize(ex::rhi::Device* device, const Shaders& shaders);
    void Shutdown();

    bool Reset(const EulerConfig& config);
    // Solid boxes for the flow-around variant. The mask is rebuilt on the next
    // Reset/Rescale and immediately when the domain is already valid.
    void SetObstacles(const std::vector<sim::ObstacleBox>& obstacles);
    // Dynamic bodies sampled for the 4C drag coupling.
    void SetFlowBodies(const std::vector<sim::FlowBody>& bodies);
    void ConsumeBodyFlow(std::vector<sim::BodyFlowSample>& samples);
    void Step(int steps);
    void UpdateRenderVolume();
    // Precomputes the sun transmittance volume for the volume raymarcher.
    void UpdateLightVolume(const DirectX::XMFLOAT3& sunDirection);
    // Moves the render/light volumes into the shader-read layout expected by
    // the graphics passes. The solver transitions them back to GENERAL before
    // its next write.
    void TransitionToShaderRead();
    bool RescaleDomain(float factor);

    // Headless captures wait for the reduction so the time-step sequence is
    // reproducible; the RHI backend always waits for its submission fence, so
    // both modes share the same code path.
    void SetDeterministic(bool deterministic) { deterministic_ = deterministic; }

    // GPU timestamp timing for the solver-only benchmark. BeginTiming
    // allocates queries for at most `maxPasses` passes, ResolveTiming blocks
    // once to read the results. Outside the benchmark no queries are created.
    bool BeginTiming(int maxPasses);
    void EndTiming();
    SolverTiming ResolveTiming();

    bool PollReduction();
    bool WaitForReduction();

    void SetPlumeModel(float radius, float centerY, float top, float dragRadius,
                       float riseSpeed) {
        plumeRadius_ = radius > 0.0f ? radius : 0.0f;
        plumeCenterY_ = centerY;
        plumeTop_ = top > 0.0f ? top : 0.0f;
        dragRadius_ = dragRadius > 0.0f ? dragRadius : plumeRadius_;
        riseSpeed_ = riseSpeed > 0.0f ? riseSpeed : 0.0f;
    }

    double ConsumeAdvance();

    bool HasVolume() const { return hasVolume_; }
    ex::rhi::Texture* RenderTexture() const { return renderVolume_.get(); }
    ex::rhi::Texture* LightTexture() const { return lightVolume_.get(); }
    const sim::VolumeBounds& Bounds() const { return bounds_; }
    float ExtinctionScale() const { return extinctionScale_; }
    const SolverStats& Stats() const { return stats_; }

    bool HasFieldBounds() const { return stats_.fieldBoundsValid; }
    float FieldTop() const { return stats_.plumeTop; }
    float FieldRadius() const { return stats_.plumeRadius; }
    float FieldCapRadius() const { return stats_.plumeCapRadius; }
    float FieldDomeFraction() const { return stats_.plumeDomeFraction; }

private:
    struct Constants {
        DirectX::XMINT4 grid;
        DirectX::XMFLOAT4 domainMin;
        DirectX::XMFLOAT4 domainMax;
        DirectX::XMFLOAT4 ambient;
        DirectX::XMFLOAT4 charge;
        DirectX::XMFLOAT4 physics;
        DirectX::XMFLOAT4 extra;
        DirectX::XMFLOAT4 oldDomainMin;
        DirectX::XMFLOAT4 oldDomainMax;
        DirectX::XMFLOAT4 sunDirection;
        DirectX::XMFLOAT4 lightParams;
        DirectX::XMFLOAT4 capParams;
        DirectX::XMFLOAT4 envParams;
    };

    using SetPtr = std::unique_ptr<ex::rhi::DescriptorSet>;

    bool CreatePipeline(const ShaderSource& source, const char* name,
                        const ex::rhi::DescriptorBinding* bindings,
                        uint32_t bindingCount,
                        std::unique_ptr<ex::rhi::Shader>& shader,
                        std::unique_ptr<ex::rhi::Pipeline>& pipeline);
    bool CreateTextures(int resolution);
    bool CreateSolidMask();
    bool CreateBodyBuffers();
    void DispatchBodyFlow();
    void ReleaseVolumeResources();
    bool CreateDescriptorSets();
    void UpdateConstants(int direction, float dt);
    void DispatchGrid();
    void DispatchSweep(int direction, bool applySource);
    void DispatchReduce();
    void DispatchRender();
    void ConsumeStaging();
    void BeginPass(SolverPass pass);
    void EndPass();

    static constexpr float kBoundScale = 512.0f; // must match euler_reduce.hlsl
    static constexpr int kMaxBatchSteps = 8;
    static constexpr int kLightVolumeResolution = 64;
    static constexpr int kReduceChannels = 8;
    static constexpr int kMaxFlowBodies = 4096;
    static constexpr int kThreadGroup = 8; // EULER_GROUP_* default in the shaders

    ex::rhi::Device* device_ = nullptr;
    std::unique_ptr<ex::rhi::CommandList> commandList_;

    std::unique_ptr<ex::rhi::Shader> initShader_;
    std::unique_ptr<ex::rhi::Shader> sweepShaders_[3];
    std::unique_ptr<ex::rhi::Shader> sweepSourceShaders_[3];
    std::unique_ptr<ex::rhi::Shader> reduceShader_;
    std::unique_ptr<ex::rhi::Shader> renderShader_;
    std::unique_ptr<ex::rhi::Shader> lightShader_;
    std::unique_ptr<ex::rhi::Shader> bodyflowShader_;
    std::unique_ptr<ex::rhi::Shader> rescaleShader_;

    std::unique_ptr<ex::rhi::Pipeline> initPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> sweepPipelines_[3];
    std::unique_ptr<ex::rhi::Pipeline> sweepSourcePipelines_[3];
    std::unique_ptr<ex::rhi::Pipeline> reducePipeline_;
    std::unique_ptr<ex::rhi::Pipeline> renderPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> lightPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> bodyflowPipeline_;
    std::unique_ptr<ex::rhi::Pipeline> rescalePipeline_;

    // One set per ping-pong parity; the state textures stay in GENERAL so a
    // set never has to be rewritten after creation.
    SetPtr initSet_;
    SetPtr sweepSets_[3][2];
    SetPtr sweepSourceSets_[3][2];
    SetPtr reduceSets_[2];
    SetPtr renderSets_[2];
    SetPtr rescaleSets_[2];
    SetPtr lightSet_;
    SetPtr bodyFlowSets_[2];

    std::unique_ptr<ex::rhi::Buffer> constantsBuffer_;
    Constants constants_{};
    std::unique_ptr<ex::rhi::Sampler> linearSampler_;
    std::unique_ptr<ex::rhi::Buffer> reduceBuffer_;

    std::unique_ptr<ex::rhi::Texture> stateA_[2];
    std::unique_ptr<ex::rhi::Texture> stateB_[2];
    std::unique_ptr<ex::rhi::Texture> renderVolume_;
    std::unique_ptr<ex::rhi::Texture> lightVolume_;
    std::unique_ptr<ex::rhi::Texture> solidMask_;
    std::unique_ptr<ex::rhi::Buffer> bodyPositionBuffer_;
    std::unique_ptr<ex::rhi::Buffer> bodyFlowBuffer_;
    int bodyCount_ = 0;
    std::vector<sim::ObstacleBox> obstacles_;
    int solidMaskResolution_ = 0;
    int stateIndex_ = 0;
    int resolution_ = 0;

    bool reducePending_ = false;
    bool deterministic_ = false;

    // GPU timestamp pool, allocated lazily by BeginTiming.
    std::unique_ptr<ex::rhi::QueryPool> timingPool_;
    std::vector<uint8_t> timingLabels_;
    int timingQueryLimit_ = 0;
    int timingIndex_ = 0;
    int timingOpenPass_ = -1;
    bool timingActive_ = false;

    EulerConfig config_{};
    sim::VolumeBounds bounds_{};
    sim::VolumeBounds oldBounds_{};
    float dx_ = 0.1f;
    float oldDx_ = 0.1f;
    float initialDx_ = 0.1f;
    int expansions_ = 0;
    float depositRadius_ = 0.1f;
    float extinctionScale_ = 1.0f;
    double time_ = 0.0;
    double advance_ = 0.0;
    float maxSpeed_ = 4000.0f;
    float lastDt_ = 0.0f;
    float plumeRadius_ = 0.0f;
    float plumeCenterY_ = 0.0f;
    float plumeTop_ = 0.0f;
    float dragRadius_ = 0.0f;
    float riseSpeed_ = 0.0f;
    float stableLayerY_ = 0.0f;
    float initialTop_ = 0.0f;
    float capThreshold_ = 0.0f;
    DirectX::XMFLOAT3 sunDirection_{0.0f, 1.0f, 0.0f};
    long long totalSteps_ = 0;
    bool hasVolume_ = false;
    bool initialized_ = false;
    SolverStats stats_{};
};

} // namespace ex::gfx
