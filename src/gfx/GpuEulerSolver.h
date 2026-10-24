#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <cstdint>

#include "gfx/GfxDevice.h"
#include "sim/VolumeBaker.h"

namespace ex::gfx {

// Configuration of the GPU compressible-flow solver. The domain is a cube of
// side 2 * domainHalfExtent with the ground at y = 0.
struct EulerConfig {
    int resolution = 128;
    double domainHalfExtent = 2.1;
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
};

// Finite-volume compressible Euler solver running on the GPU (D3D11 compute).
// HLLC Riemann solver, MUSCL/minmod reconstruction, dimensional splitting,
// buoyancy and soot cooling. The state is converted into the volume field used
// by the existing raymarcher.
class GpuEulerSolver {
public:
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    uint64_t dedicatedVideoMemoryBytes);
    void Shutdown();

    bool Reset(const EulerConfig& config);
    void Step(int steps);
    void UpdateRenderVolume();

    // Simulated time advanced since the last call (used to keep the reduced
    // order model and the particles in sync with the solver).
    double ConsumeAdvance();

    bool HasVolume() const { return hasVolume_; }
    ID3D11ShaderResourceView* VolumeSRV() const { return renderVolume_.srv.Get(); }
    const sim::VolumeBounds& Bounds() const { return bounds_; }
    float ExtinctionScale() const { return extinctionScale_; }
    const SolverStats& Stats() const { return stats_; }

private:
    struct Constants {
        DirectX::XMINT4 grid;
        DirectX::XMFLOAT4 domainMin;
        DirectX::XMFLOAT4 domainMax;
        DirectX::XMFLOAT4 ambient;
        DirectX::XMFLOAT4 charge;
        DirectX::XMFLOAT4 physics;
        DirectX::XMFLOAT4 extra;
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
    void DispatchSource(float dt);
    void DispatchReduce();
    void DispatchRender();
    float ReadMaxSpeed();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    uint64_t videoMemoryBytes_ = 0;

    ComPtr<ID3D11ComputeShader> initShader_;
    ComPtr<ID3D11ComputeShader> sweepShader_;
    ComPtr<ID3D11ComputeShader> sourceShader_;
    ComPtr<ID3D11ComputeShader> reduceShader_;
    ComPtr<ID3D11ComputeShader> renderShader_;

    ComPtr<ID3D11Buffer> constantBuffer_;
    Constants constants_{};

    VolumeTexture stateA_[2];
    VolumeTexture stateB_[2];
    VolumeTexture renderVolume_;
    int stateIndex_ = 0;
    int resolution_ = 0;

    ComPtr<ID3D11Buffer> maxSpeedBuffer_;
    ComPtr<ID3D11UnorderedAccessView> maxSpeedUAV_;
    ComPtr<ID3D11Buffer> maxSpeedStaging_;
    bool stagingReady_ = false;

    EulerConfig config_{};
    sim::VolumeBounds bounds_{};
    float dx_ = 0.1f;
    float depositRadius_ = 0.1f;
    float extinctionScale_ = 1.0f;
    double time_ = 0.0;
    double advance_ = 0.0;
    float maxSpeed_ = 4000.0f;
    float lastDt_ = 0.0f;
    long long totalSteps_ = 0;
    bool hasVolume_ = false;
    bool initialized_ = false;
    SolverStats stats_{};
};

} // namespace ex::gfx
