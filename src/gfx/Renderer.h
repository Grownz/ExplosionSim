#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <string>
#include <vector>

#include "gfx/Camera.h"
#include "gfx/GfxDevice.h"
#include "gfx/GpuEulerSolver.h"
#include "gfx/SceneRenderer.h"
#include "sim/Simulation.h"

namespace ex::gfx {

struct RenderSettings {
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
    int anisotropicFiltering = 0; // 0 = off, 2/4/8 = sample count
};

struct FrameInfo {
    int additiveParticles = 0;
    int fireParticles = 0;
    int alphaParticles = 0;
    int raymarchSteps = 0;
    bool volumeValid = false;
    bool highFidelity = false;
    float cpuRenderMs = 0.0f;
};

// Result of the solver-only benchmark: GPU pass timings from timestamp queries
// plus the wall-clock time of the measured loop.
struct SolverBenchmarkResult {
    bool valid = false;
    int resolution = 0;
    long long steps = 0;
    int stepsPerFrame = 0;
    int frames = 0;
    double wallMs = 0.0;
    // CPU time spent submitting solver work, excluding reduction waits.
    double submissionMs = 0.0;
    double msPerStep = 0.0;
    double msPerFrame = 0.0;
    float domainHalfExtent = 0.0f;
    float voxelSize = 0.0f;
    SolverTiming timing;
};

class Renderer {
public:
    bool Initialize(GfxDevice& device);
    void Shutdown();
    void Resize(int width, int height);
    void Render(sim::Simulation& simulation, const Camera& camera, const RenderSettings& settings);
    void SetSolverPaused(bool paused) { solverPaused_ = paused; }

    // Advances the high-fidelity solver so its simulated time matches the
    // playback time scale for this frame. Returns the advanced simulation time.
    double AdvanceSolver(sim::Simulation& simulation, float realDt);

    // Advances the GPU solver to a target simulation time (headless captures).
    void PrepareHighFidelity(sim::Simulation& simulation, double targetTime);

    // Advances the GPU solver to an exact step count, keeping the reduced-order
    // model (camera and auto-scale trigger) in sync (headless captures).
    void PrepareHighFidelitySteps(sim::Simulation& simulation, long long targetSteps);

    // Simulated time advanced by the solver during the last frame.
    double ConsumeSolverAdvance();
    void StepSolverOnce(int steps = 8);

    // Solver-only benchmark: steps the GPU solver without rendering the scene
    // and returns per-pass GPU timings. Uses the non-blocking reduction path so
    // the numbers match interactive use.
    SolverBenchmarkResult BenchmarkSolver(sim::Simulation& simulation, long long steps,
                                          int stepsPerFrame);
    const SolverStats& Solver() const { return solver_.Stats(); }
    bool SolverBudgetHit() const { return solverBudgetHit_; }

    const FrameInfo& Info() const { return info_; }
    int VolumeTextureResolution() const { return volumeResolution_; }
    // True when the compute-shader volume bake compiled and is available.
    bool SupportsGpuVolumeBake() const { return volumeBakeCS_ != nullptr; }
    bool GpuVolumeBakeActive() const { return gpuVolumeBakeActive_; }
    // True when the GPU particle compute shader compiled.
    bool SupportsGpuParticles() const { return particleIntegrateCS_ != nullptr; }
    // Clears the bloom mip chain. Called when the simulation is rearmed or
    // restarted so the previous explosion's glow does not linger.
    void ResetBloom();
    // Advances the GPU particle simulation outside Render(). Headless captures
    // call this after every fixed step so the particles evolve with the
    // simulation instead of only once per rendered frame.
    void StepGpuParticles(sim::Simulation& simulation);

private:
    struct Target {
        std::unique_ptr<rhi::Texture> texture;
        int width = 0;
        int height = 0;

        ID3D11Texture2D* NativeTexture() const {
            return texture
                       ? static_cast<ID3D11Texture2D*>(texture->NativeHandle())
                       : nullptr;
        }
        ID3D11RenderTargetView* RTV() const {
            return texture ? static_cast<ID3D11RenderTargetView*>(
                                 texture->NativeRenderTargetView())
                           : nullptr;
        }
        ID3D11ShaderResourceView* SRV() const {
            return texture ? static_cast<ID3D11ShaderResourceView*>(
                                 texture->NativeShaderResourceView())
                           : nullptr;
        }

        void Reset() {
            texture.reset();
            width = 0;
            height = 0;
        }
    };

    struct FrameConstants {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT4 cameraPosition;
        DirectX::XMFLOAT4 sunDirection;
        DirectX::XMFLOAT4 sunColor;
        DirectX::XMFLOAT4 volumeMin;
        DirectX::XMFLOAT4 volumeMax;
        DirectX::XMFLOAT4 screenParams;
        DirectX::XMFLOAT4 misc;
        DirectX::XMFLOAT4 misc2;
        DirectX::XMFLOAT4 misc3;
        DirectX::XMFLOAT4 wind;
        DirectX::XMFLOAT4 cameraRight;
        DirectX::XMFLOAT4 cameraUp;
        DirectX::XMFLOAT4 gridParams;
        DirectX::XMFLOAT4 rangeParams;
        DirectX::XMFLOAT4 fireball;
        DirectX::XMFLOAT4 volumeParams;
        DirectX::XMFLOAT4 volumeParams2;
        DirectX::XMFLOAT4 lightVolumeMin;
        DirectX::XMFLOAT4 lightVolumeMax;
        DirectX::XMFLOAT4 blast;
    };

    struct BloomConstants {
        DirectX::XMFLOAT4 texelSize;
        DirectX::XMFLOAT4 bloomParams;
    };

    // Must match the cbuffer in volume_bake.hlsl.
    struct VolumeBakeConstants {
        DirectX::XMFLOAT4 boundsMin;   // w = resolution
        DirectX::XMFLOAT4 boundsSize;  // w = voxel size
        DirectX::XMFLOAT4 fireball;    // w = radius
        DirectX::XMFLOAT4 fireball2;   // x temp, y visible, z time, w ambient
        DirectX::XMFLOAT4 cloud;       // w = radius
        DirectX::XMFLOAT4 cloud2;      // x temp, y stem, z strength, w dust ring
        DirectX::XMFLOAT4 shock;       // x radius, y mach, z burstY, w dust intensity
        DirectX::XMFLOAT4 maskParams;  // x = solid mask enabled
        DirectX::XMFLOAT4 maskMin;     // xyz = world-space mask box minimum
        DirectX::XMFLOAT4 maskMax;     // xyz = world-space mask box maximum
    };

    // Must match the cbuffer in particles_gpu.hlsl.
    struct ParticleUpdateConstants {
        DirectX::XMFLOAT4 windTime;  // xyz = wind, w = time
        DirectX::XMFLOAT4 params;    // x = ambient T, y = gravity, z = dt, w = capacity
        DirectX::XMFLOAT4 counts;    // x = spawn count
    };

    bool CreatePipeline();
    bool CreateStates();
    bool CreateNoiseTexture();
    bool InitializeSceneRenderer();
    bool EnsureTargets(int width, int height);
    bool CreateTarget(Target& target, int width, int height);
    EulerConfig MakeEulerConfig(const sim::Simulation& simulation) const;
    bool EnsureSolver(sim::Simulation& simulation);
    void MaybeExpandSolver(const sim::Simulation& simulation);
    void UpdatePlumeModel(sim::Simulation& simulation);
    bool EnsureVolumeTexture(int resolution, bool halfFloat, bool split);
    void UploadVolume(const sim::VolumeFrame& frame);
    bool DispatchGpuVolumeBake(const sim::VolumeBakeRequest& request);
    bool EnsureSolidMask(const sim::VolumeBakeRequest& request);
    bool CreateParticleGpuResources(int capacity);
    void UpdateGpuParticles(sim::Simulation& simulation);
    void ReadParticleCounters(sim::ParticleSystem& particles);    void UploadParticles(const std::vector<sim::ParticleInstance>& instances,
                         ComPtr<ID3D11Buffer>& buffer);
    void UpdateFrameConstants(const sim::Simulation& simulation, const Camera& camera,
                              const RenderSettings& settings);
    void UpdateBloomConstants(float sourceWidth, float sourceHeight, float threshold, float intensity);
    void RenderBloom();
    void SetViewport(int width, int height);

    GfxDevice* device_ = nullptr;
    int frameIndex_ = 0;
    double lastSimTime_ = 0.0;

    Target hdr_;
    // Snapshot of the HDR buffer for the shock-front refraction pass, which
    // samples the scene while writing back into hdr_.
    Target hdrCopy_;
    static constexpr int kBloomLevels = 5;
    Target bloom_[kBloomLevels];

    ComPtr<ID3D11Texture3D> volumeTexture_;
    ComPtr<ID3D11ShaderResourceView> volumeSRV_;
    ComPtr<ID3D11UnorderedAccessView> volumeUAV_;
    ComPtr<ID3D11Texture3D> volumeAuxTexture_;
    ComPtr<ID3D11ShaderResourceView> volumeAuxSRV_;
    ComPtr<ID3D11ShaderResourceView> volumeDummySRV_;
    ComPtr<ID3D11ComputeShader> volumeBakeCS_;
    ComPtr<ID3D11Buffer> volumeBakeConstantBuffer_;
    ComPtr<ID3D11Texture3D> solidMaskTexture_;
    ComPtr<ID3D11ShaderResourceView> solidMaskSRV_;
    uint64_t solidMaskVersion_ = 0;
    sim::VolumeBounds gpuVolumeBounds_{};
    float gpuVolumeExtinction_ = 0.1f;
    int volumeResolution_ = 0;
    bool volumeHalfFloat_ = true;
    bool volumeSplit_ = false;
    bool volumeValid_ = false;
    bool gpuVolumeBakeActive_ = false;

    // Active volume source for the current frame (CPU bake or GPU solver).
    sim::VolumeBounds activeVolumeBounds_{};
    float activeVolumeExtinction_ = 0.1f;
    bool activeVolumeValid_ = false;
    bool activeVolumeSplit_ = false;

    GpuEulerSolver solver_;
    SceneRenderer sceneRenderer_;
    uint64_t solverBlastSerial_ = ~uint64_t(0);
    bool solverPrepared_ = false;
    bool solverCapWarned_ = false;
    bool solverPaused_ = false;
    bool solverBudgetHit_ = false;
    int solverObstacleFrame_ = 0;

    // Copy of the depth buffer that can be sampled while the original is still
    // bound as a depth-stencil target.
    ComPtr<ID3D11Texture2D> depthCopyTexture_;
    ComPtr<ID3D11ShaderResourceView> depthCopySRV_;

    ComPtr<ID3D11Buffer> frameConstantBuffer_;
    ComPtr<ID3D11Buffer> bloomConstantBuffer_;
    ComPtr<ID3D11Buffer> shadowConstantBuffer_;
    FrameConstants frameConstants_{};
    BloomConstants bloomConstants_{};

    struct ShadowConstants {
        DirectX::XMFLOAT4X4 lightViewProj[3]{};
        // Camera-independent world-space projection for the cached static
        // casters (intact buildings).
        DirectX::XMFLOAT4X4 staticLightViewProj{};
        DirectX::XMFLOAT4 cascadeSplits{};
        DirectX::XMFLOAT4 cascadeTexels{};      // 1 / shadow map size
        DirectX::XMFLOAT4 cascadeWorldTexels{}; // world size of one texel
        DirectX::XMFLOAT4 staticTexel{};        // x = 1/size, y = world texel, z = depth bias
    };
    ShadowConstants shadowConstants_{};

    ComPtr<ID3D11Buffer> particleBufferAdditive_;
    ComPtr<ID3D11ShaderResourceView> particleSRVAdditive_;
    ComPtr<ID3D11Buffer> particleBufferFire_;
    ComPtr<ID3D11ShaderResourceView> particleSRVFire_;
    ComPtr<ID3D11Buffer> particleBufferAlpha_;
    ComPtr<ID3D11ShaderResourceView> particleSRVAlpha_;
    int particleCapacity_ = 0;
    bool particleGpuMode_ = false;

    // GPU particle path: state, spawn upload, batch counters and indirect args.
    ComPtr<ID3D11ComputeShader> particleSpawnCS_;
    ComPtr<ID3D11ComputeShader> particleIntegrateCS_;
    ComPtr<ID3D11ComputeShader> particleFinalizeCS_;
    ComPtr<ID3D11ComputeShader> particleResetCS_;
    ComPtr<ID3D11Buffer> particleUpdateCB_;
    ComPtr<ID3D11Buffer> particleStateBuffer_;
    ComPtr<ID3D11UnorderedAccessView> particleStateUAV_;
    ComPtr<ID3D11Buffer> particleSpawnBuffer_;
    ComPtr<ID3D11ShaderResourceView> particleSpawnSRV_;
    ComPtr<ID3D11Buffer> particleCounterBuffer_;
    ComPtr<ID3D11UnorderedAccessView> particleCounterUAV_;
    ComPtr<ID3D11Buffer> particleIndirectBuffer_;
    ComPtr<ID3D11UnorderedAccessView> particleIndirectUAV_;
    ComPtr<ID3D11Buffer> particleCounterStaging_;
    ComPtr<ID3D11Query> particleReadbackQuery_;
    bool particleReadbackPending_ = false;
    ComPtr<ID3D11UnorderedAccessView> particleUAVAdditive_;
    ComPtr<ID3D11UnorderedAccessView> particleUAVAlpha_;
    ComPtr<ID3D11UnorderedAccessView> particleUAVFire_;

    ComPtr<ID3D11VertexShader> fullscreenVS_;
    ComPtr<ID3D11PixelShader> skyPS_;
    ComPtr<ID3D11PixelShader> groundPS_;
    ComPtr<ID3D11PixelShader> volumePS_;
    ComPtr<ID3D11VertexShader> particleVS_;
    ComPtr<ID3D11PixelShader> particleAdditivePS_;
    ComPtr<ID3D11PixelShader> particleAlphaPS_;
    ComPtr<ID3D11PixelShader> particleFirePS_;
    ComPtr<ID3D11PixelShader> shockPS_;
    ComPtr<ID3D11PixelShader> downsamplePS_;
    ComPtr<ID3D11PixelShader> upsamplePS_;
    ComPtr<ID3D11PixelShader> compositePS_;

    ComPtr<ID3D11SamplerState> linearClamp_;
    ComPtr<ID3D11SamplerState> linearWrap_;
    ComPtr<ID3D11SamplerState> anisoClamp_[3]; // 2x, 4x, 8x
    ComPtr<ID3D11SamplerState> anisoWrap_[3];  // 2x, 4x, 8x
    // RHI samplers for the migrated scene pass.
    std::unique_ptr<rhi::Sampler> surfaceSamplerLinear_;
    std::unique_ptr<rhi::Sampler> surfaceSamplerAniso_[3];
    ComPtr<ID3D11SamplerState> noiseSampler_;
    ComPtr<ID3D11Texture3D> noiseTexture_;
    ComPtr<ID3D11ShaderResourceView> noiseSRV_;
    ComPtr<ID3D11BlendState> blendOpaque_;
    ComPtr<ID3D11BlendState> blendAlpha_;
    ComPtr<ID3D11BlendState> blendAdditive_;
    ComPtr<ID3D11BlendState> blendPremultiplied_;
    ComPtr<ID3D11DepthStencilState> depthDisabled_;
    ComPtr<ID3D11DepthStencilState> depthTest_;
    ComPtr<ID3D11DepthStencilState> depthWrite_;
    ComPtr<ID3D11RasterizerState> rasterizer_;

    FrameInfo info_{};
};

} // namespace ex::gfx
