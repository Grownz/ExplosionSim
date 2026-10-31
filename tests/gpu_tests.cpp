// Smoke test for the high-fidelity GPU Euler solver.
//
// The test creates its own Direct3D 11 device (hardware first, then WARP). If no
// device is available it is skipped, so CI machines without a GPU still pass.
// Shader or resource failures are real errors.

#include "gfx/GpuEulerSolver.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("FAILED: %s (line %d)\n", expression, line);
        ++g_failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)
#define CHECK_NEAR(value, expected, tolerance)                                              \
    Check(std::abs(double(value) - double(expected)) <= double(tolerance),                  \
          #value " ~ " #expected, __LINE__)

bool CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                   D3D11_SDK_VERSION, &device, &obtained, &context);
    if (FAILED(hr)) {
        device.Reset();
        context.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                               D3D11_SDK_VERSION, &device, &obtained, &context);
    }
    return SUCCEEDED(hr) && obtained >= D3D_FEATURE_LEVEL_11_0;
}

} // namespace

int main() {
    // Domain sizing regression: a high burst must fit inside [0, 2 * half],
    // otherwise the solver clamps the charge to the domain ceiling and the
    // resolved cloud sits far below the analytic shock front.
    const double highHalf = ex::gfx::SolverHalfExtentForBurst(400.0, 19.83);
    CHECK(2.0 * highHalf >= 400.0 + 2.0 * 19.83);
    CHECK_NEAR(ex::gfx::SolverHalfExtentForBurst(0.4, 0.6), 1.8, 1e-9);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(device, context)) {
        std::printf("GPU test skipped: no Direct3D 11 device available.\n");
        return 0;
    }

    ex::gfx::GpuEulerSolver solver;
    if (!solver.Initialize(device.Get(), context.Get(), 0)) {
        std::printf("FAILED: solver initialisation (shaders or resources)\n");
        return 1;
    }

    ex::gfx::EulerConfig config;
    config.resolution = 32;
    config.domainHalfExtent = 2.0;
    config.chargeMassKg = 0.2;
    config.chargeEnergyJ = 8.37e5;
    config.heightOfBurstM = 0.4;
    config.ambientDensity = 1.225;
    config.ambientPressure = 101325.0;
    config.ambientTemperature = 288.15;
    config.gamma = 1.4;
    config.gasConstant = 287.058;
    config.gravity = 9.80665;
    config.fireballRadiusM = 0.6;

    CHECK(solver.Reset(config));
    solver.SetDeterministic(true);
    CHECK(solver.HasVolume());
    CHECK(solver.Stats().steps == 0);

    solver.Step(8);
    CHECK(solver.Stats().steps == 8);
    CHECK(solver.Stats().time > 0.0);
    CHECK(std::isfinite(solver.Stats().maxSpeed));
    CHECK(solver.Stats().maxSpeed > 0.0f);


    solver.Step(8);
    CHECK(solver.Stats().steps == 16);
    CHECK(solver.Stats().fieldBoundsValid);
    CHECK(solver.Stats().plumeTop > 0.0f);

    const ex::sim::VolumeBounds before = solver.Bounds();
    const float halfBefore = 0.5f * (before.max.x - before.min.x);
    CHECK(solver.RescaleDomain(1.3f));
    const ex::sim::VolumeBounds after = solver.Bounds();
    const float halfAfter = 0.5f * (after.max.x - after.min.x);
    CHECK_NEAR(halfAfter, halfBefore * 1.3f, halfBefore * 0.01f);
    CHECK_NEAR(solver.Stats().accuracyFactor, 1.3f, 0.01f);
    CHECK(solver.Stats().expansions == 1);

    solver.Step(8);
    CHECK(solver.Stats().steps == 24);
    CHECK(solver.Stats().fieldBoundsValid);

    // 4C drag sampling: a body at the burst sees a dense, moving flow.
    std::vector<ex::sim::FlowBody> flowBodies(1);
    flowBodies[0].position = DirectX::XMFLOAT3(0.0f, 0.4f, 0.0f);
    flowBodies[0].physicsIndex = 0;
    solver.SetFlowBodies(flowBodies);
    solver.Step(1);
    std::vector<ex::sim::BodyFlowSample> flowSamples;
    solver.ConsumeBodyFlow(flowSamples);
    CHECK(flowSamples.size() == 1);
    CHECK(flowSamples[0].density > 0.0f);
    CHECK(std::isfinite(flowSamples[0].velocity.x));

    solver.Shutdown();

    if (g_failures == 0) {
        std::printf("GPU solver tests passed.\n");
        return 0;
    }
    std::printf("%d GPU test(s) failed.\n", g_failures);
    return 1;
}
