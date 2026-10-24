#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <string>
#include <vector>

#include "gfx/Camera.h"
#include "gfx/GfxDevice.h"
#include "gfx/GpuEulerSolver.h"
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

class Renderer {
public:
    bool Initialize(GfxDevice& device);
    void Shutdown();
    void Resize(int width, int height);
    void Render(sim::Simulation& simulation, const Camera& camera, const RenderSettings& settings);
    void SetSolverPaused(bool paused) { solverPaused_ = paused; }

    // Advances the GPU solver to a target simulation time (headless captures).
    void PrepareHighFidelity(sim::Simulation& simulation, double targetTime);

    // Simulated time advanced by the solver during the last frame.
    double ConsumeSolverAdvance();
    void StepSolverOnce(int steps = 8);
    const SolverStats& Solver() const { return solver_.Stats(); }

    const FrameInfo& Info() const { return info_; }
    int VolumeTextureResolution() const { return volumeResolution_; }

private:
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0;
        int height = 0;

        void Reset() {
            srv.Reset();
            rtv.Reset();
            texture.Reset();
            width = 0;
            height = 0;
        }
    };

    struct alignas(16) FrameConstants {
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
    };

    struct alignas(16) BloomConstants {
        DirectX::XMFLOAT4 texelSize;
        DirectX::XMFLOAT4 bloomParams;
    };

    bool CreatePipeline();
    bool CreateStates();
    bool CreateNoiseTexture();
    bool EnsureTargets(int width, int height);
    bool CreateTarget(Target& target, int width, int height);
    EulerConfig MakeEulerConfig(const sim::Simulation& simulation) const;
    void UploadVolume(const sim::VolumeFrame& frame);
    void UploadParticles(const std::vector<sim::ParticleInstance>& instances,
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
    static constexpr int kBloomLevels = 5;
    Target bloom_[kBloomLevels];

    ComPtr<ID3D11Texture3D> volumeTexture_;
    ComPtr<ID3D11ShaderResourceView> volumeSRV_;
    ComPtr<ID3D11Texture3D> volumeAuxTexture_;
    ComPtr<ID3D11ShaderResourceView> volumeAuxSRV_;
    ComPtr<ID3D11ShaderResourceView> volumeDummySRV_;
    int volumeResolution_ = 0;
    bool volumeHalfFloat_ = true;
    bool volumeSplit_ = false;
    bool volumeValid_ = false;

    // Active volume source for the current frame (CPU bake or GPU solver).
    sim::VolumeBounds activeVolumeBounds_{};
    float activeVolumeExtinction_ = 0.1f;
    bool activeVolumeValid_ = false;
    bool activeVolumeSplit_ = false;

    GpuEulerSolver solver_;
    uint64_t solverBlastSerial_ = ~uint64_t(0);
    bool solverPrepared_ = false;
    bool solverPaused_ = false;

    // Copy of the depth buffer that can be sampled while the original is still
    // bound as a depth-stencil target.
    ComPtr<ID3D11Texture2D> depthCopyTexture_;
    ComPtr<ID3D11ShaderResourceView> depthCopySRV_;

    ComPtr<ID3D11Buffer> frameConstantBuffer_;
    ComPtr<ID3D11Buffer> bloomConstantBuffer_;
    FrameConstants frameConstants_{};
    BloomConstants bloomConstants_{};

    ComPtr<ID3D11Buffer> particleBufferAdditive_;
    ComPtr<ID3D11ShaderResourceView> particleSRVAdditive_;
    ComPtr<ID3D11Buffer> particleBufferFire_;
    ComPtr<ID3D11ShaderResourceView> particleSRVFire_;
    ComPtr<ID3D11Buffer> particleBufferAlpha_;
    ComPtr<ID3D11ShaderResourceView> particleSRVAlpha_;
    int particleCapacity_ = 0;

    ComPtr<ID3D11VertexShader> fullscreenVS_;
    ComPtr<ID3D11PixelShader> skyPS_;
    ComPtr<ID3D11PixelShader> groundPS_;
    ComPtr<ID3D11PixelShader> volumePS_;
    ComPtr<ID3D11VertexShader> particleVS_;
    ComPtr<ID3D11PixelShader> particleAdditivePS_;
    ComPtr<ID3D11PixelShader> particleAlphaPS_;
    ComPtr<ID3D11PixelShader> downsamplePS_;
    ComPtr<ID3D11PixelShader> upsamplePS_;
    ComPtr<ID3D11PixelShader> compositePS_;

    ComPtr<ID3D11SamplerState> linearClamp_;
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
