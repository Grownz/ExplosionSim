// Smoke test for the RHI (Vulkan) high-fidelity Euler solver.
//
// The test creates an offscreen Vulkan device. If no compatible device is
// available it is skipped, so CI machines without Vulkan still pass. Shader or
// resource failures are real errors.

#include "app/EulerInitShader.h"
#include "app/EulerLightShader.h"
#include "app/EulerReduceShader.h"
#include "app/EulerRenderShader.h"
#include "app/EulerRescaleShader.h"
#include "app/EulerSweepShader.h"
#include "gfx/RhiEulerSolver.h"
#include "gfx/rhi/Rhi.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("FAILED: %s (line %d)\n", expression, line);
        ++g_failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)
#define CHECK_NEAR(value, expected, tolerance) \
    Check(std::abs(double(value) - double(expected)) <= double(tolerance), \
          #value " ~ " #expected, __LINE__)

} // namespace

int main() {
    // Domain sizing regression shared with the D3D11 test.
    const double highHalf = ex::gfx::SolverHalfExtentForBurst(400.0, 19.83);
    CHECK(2.0 * highHalf >= 400.0 + 2.0 * 19.83);
    CHECK_NEAR(ex::gfx::SolverHalfExtentForBurst(0.4, 0.6), 1.8, 1e-9);

    ex::rhi::DeviceDesc desc{};
    desc.backend = ex::rhi::Backend::Vulkan;
    desc.offscreen = true;
    desc.width = 64;
    desc.height = 64;
    std::unique_ptr<ex::rhi::Device> device =
        ex::rhi::CreateVulkanDevice(desc);
    if (!device) {
        std::printf("GPU Vulkan test skipped: no Vulkan device available.\n");
        return 0;
    }

    ex::gfx::RhiEulerSolver solver;
    ex::gfx::RhiEulerSolver::Shaders shaders{
        {kEulerCSInit, kEulerCSInitSize},
        {kEulerCSSweepX, kEulerCSSweepXSize},
        {kEulerCSSweepY, kEulerCSSweepYSize},
        {kEulerCSSweepZ, kEulerCSSweepZSize},
        {kEulerCSSweepXSource, kEulerCSSweepXSourceSize},
        {kEulerCSSweepYSource, kEulerCSSweepYSourceSize},
        {kEulerCSSweepZSource, kEulerCSSweepZSourceSize},
        {kEulerCSReduce, kEulerCSReduceSize},
        {kEulerCSRender, kEulerCSRenderSize},
        {kEulerCSLight, kEulerCSLightSize},
        {kEulerCSRescale, kEulerCSRescaleSize},
    };
    if (!solver.Initialize(device.get(), shaders)) {
        std::printf("FAILED: RHI solver initialisation (shaders or resources)\n");
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

    if (!solver.Reset(config)) {
        std::printf("FAILED: RHI solver reset\n");
        return 1;
    }
    CHECK(solver.HasVolume());
    CHECK(solver.Stats().resolution == 32);
    CHECK_NEAR(solver.Bounds().max.y, 4.0, 1e-4);
    CHECK(solver.RenderTexture() != nullptr);
    CHECK(solver.LightTexture() != nullptr);

    solver.SetDeterministic(true);
    solver.SetPlumeModel(0.5f, 1.0f, 2.0f, 0.5f, 0.0f);
    solver.Step(8);
    CHECK(solver.Stats().steps == 8);
    CHECK(solver.Stats().time > 0.0);
    CHECK(std::isfinite(solver.Stats().maxSpeed));
    CHECK(solver.Stats().maxSpeed > 1.0f);

    solver.UpdateRenderVolume();
    solver.UpdateLightVolume(DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f));
    CHECK(solver.HasVolume());

    // Timestamps are optional: a device without host query reset falls back to
    // wall-clock timing in the benchmark.
    if (solver.BeginTiming(64)) {
        solver.Step(1);
        solver.UpdateRenderVolume();
        const ex::gfx::SolverTiming timing = solver.ResolveTiming();
        if (timing.valid) {
            CHECK(timing.totalMs >= 0.0);
            CHECK(timing.passCount[int(ex::gfx::SolverPass::Render)] > 0);
            std::printf("RHI solver timing: %.3f ms total\n", timing.totalMs);
        } else {
            std::printf("RHI solver timing unavailable; skipping timing checks.\n");
        }
    }

    const float oldTop = solver.Bounds().max.y;
    if (!solver.RescaleDomain(1.3f)) {
        std::printf("FAILED: RHI solver rescale\n");
        ++g_failures;
    } else {
        CHECK(solver.Bounds().max.y > oldTop);
        CHECK(solver.Stats().expansions == 1);
    }

    solver.Shutdown();

    if (g_failures == 0) {
        std::printf("All Vulkan solver tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
