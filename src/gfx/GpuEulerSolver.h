#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <cstdint>
#include <vector>

#include "gfx/GfxDevice.h"
#include "sim/VolumeBaker.h"

namespace ex::gfx {

// Configuration of the GPU compressible-flow solver. The domain is a cube of
// side 2 * domainHalfExtent with the ground at y = 0.
struct EulerConfig {
    int resolution = 128;
    double domainHalfExtent = 2.1;
    bool hemispherical = false;
    double chargeMassKg = 0.2;
    double chargeEnergyJ = 8.37e5;
    double heightOfBurstM = 0.4;
    double ambientDensity = 1.225;
    double ambientPressure = 101325.0;
    double ambientTemperature = 288.15;
    double gamma = 1.4;
    double gasConstant = 287.058;
    double gravity = 9.80665;
    double fireballRadiusM = 0.6;
};

struct SolverStats {
    bool active = false;
    int resolution = 0;
    double time = 0.0;
    double advance = 0.0;
    float dt = 0.0f;
    float maxSpeed = 0.0f;
    long long steps = 0;
    float domainHalfExtent = 0.0f;
    float domainTop = 0.0f;
    float voxelSize = 0.0f;
    float accuracyFactor = 1.0f;
    int expansions = 0;
    float plumeTop = 0.0f;
    float plumeRadius = 0.0f;
    float plumeDomeFraction = 0.0f;
    bool fieldBoundsValid = false;
};

// GPU passes the solver can time with D3D11 timestamp queries.
enum class SolverPass : int {
    SweepX = 0,
    SweepY,
    SweepZ,   // includes the fused source terms
    Reduce,
    Render,   // render-volume update
    Count
};

// Per-pass GPU timings from one BeginTiming/ResolveTiming window.
struct SolverTiming {
    bool valid = false;
    double totalMs = 0.0;
    double passMs[int(SolverPass::Count)] = {};
    int passCount[int(SolverPass::Count)] = {};
};

// Finite-volume compressible Euler solver running on the GPU (D3D11 compute).
// HLLC Riemann solver, MUSCL/minmod reconstruction, dimensional splitting, and
// buoyancy/soot cooling fused into the Z sweep. The state is converted into the
// volume field used by the existing raymarcher.
class GpuEulerSolver {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    uint64_t dedicatedVideoMemoryBytes);
    void Shutdown();

    bool Reset(const EulerConfig& config);
    void Step(int steps);
    void UpdateRenderVolume();
    bool RescaleDomain(float factor);

    // Headless captures wait for the reduction so the time step sequence is
    // reproducible; the interactive path keeps the non-blocking pipeline.
    void SetDeterministic(bool deterministic) { deterministic_ = deterministic; }

    // GPU timestamp timing for the solver-only benchmark. BeginTiming allocates
    // queries for at most `maxPasses` passes, ResolveTiming blocks once to read
    // the results. Outside the benchmark no queries are created or recorded.
    bool BeginTiming(int maxPasses);
    void EndTiming();
    SolverTiming ResolveTiming();

    // Consumes a finished reduction without blocking. Step() polls it once per
    // sub-batch; the solver benchmark also uses it at frame boundaries.
    bool PollReduction();

    // Benchmark helper: waits (bounded) for the in-flight reduction. Used at
    // frame boundaries so the benchmark's maximum speed tracks the flow like
    // the interactive path, which polls once per frame.
    bool WaitForReduction();

    // Analytic cloud radius and centre used by the sub-grid entrainment/cap
    // closure. Call before stepping with the current reduced-order cloud state.
    void SetPlumeModel(float radius, float centerY) {
        plumeRadius_ = radius > 0.0f ? radius : 0.0f;
        plumeCenterY_ = centerY;
    }

    // Simulated time advanced since the last call (used to keep the reduced
    // order model and the particles in sync with the solver).
    double ConsumeAdvance();

    bool HasVolume() const { return hasVolume_; }
    ID3D11ShaderResourceView* VolumeSRV() const { return renderVolume_.srv.Get(); }
    const sim::VolumeBounds& Bounds() const { return bounds_; }
    float ExtinctionScale() const { return extinctionScale_; }
    const SolverStats& Stats() const { return stats_; }

    // Measured extent of the visible product field (world units). Valid after
    // at least one reduction pass; reset on Reset().
    bool HasFieldBounds() const { return stats_.fieldBoundsValid; }
    float FieldTop() const { return stats_.plumeTop; }
    float FieldRadius() const { return stats_.plumeRadius; }
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
    };

    struct VolumeTexture {
        ComPtr<ID3D11Texture3D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;

        void Reset() {
            uav.Reset();
            srv.Reset();
            texture.Reset();
        }
    };

    bool CreateComputeShaders();
    bool CreateTextures(int resolution);
    bool CreateAuxiliaryResources();
    void ReleaseVolumeResources();
    void UpdateConstants(int direction, float dt);
    void BindStateSRV(int index);
    void BindStateUAV(int index);
    void UnbindAll();
    void Dispatch(int groups);
    void DispatchSweep(int direction, float dt);
    void DispatchReduce();
    void DispatchRender();
    void BeginPass(SolverPass pass);
    void EndPass();
    void ConsumeStaging();

    static constexpr float kBoundScale = 512.0f; // must match euler_reduce.hlsl
    static constexpr int kMaxBatchSteps = 8;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    uint64_t videoMemoryBytes_ = 0;

    ComPtr<ID3D11ComputeShader> initShader_;
    ComPtr<ID3D11ComputeShader> sweepShader_;
    ComPtr<ID3D11ComputeShader> sweepZShader_;
    ComPtr<ID3D11ComputeShader> reduceShader_;
    ComPtr<ID3D11ComputeShader> renderShader_;
    ComPtr<ID3D11ComputeShader> rescaleShader_;

    ComPtr<ID3D11Buffer> constantBuffer_;
    Constants constants_{};
    ComPtr<ID3D11SamplerState> linearSampler_;

    VolumeTexture stateA_[2];
    VolumeTexture stateB_[2];
    VolumeTexture renderVolume_;
    int stateIndex_ = 0;
    int resolution_ = 0;

    ComPtr<ID3D11Buffer> maxSpeedBuffer_;
    ComPtr<ID3D11UnorderedAccessView> maxSpeedUAV_;
    ComPtr<ID3D11Buffer> maxSpeedStaging_;
    ComPtr<ID3D11Query> reduceQuery_;
    bool reducePending_ = false;
    bool deterministic_ = false;

    // GPU timestamp pool, allocated lazily by BeginTiming.
    ComPtr<ID3D11Query> timingDisjoint_;
    std::vector<ComPtr<ID3D11Query>> timingQueries_;
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
    long long totalSteps_ = 0;
    bool hasVolume_ = false;
    bool initialized_ = false;
    SolverStats stats_{};
};

} // namespace ex::gfx
