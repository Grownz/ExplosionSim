#pragma once

#include <algorithm>
#include <cstdint>

namespace ex::gfx {

// Minimum cubic half extent that keeps a burst at `heightOfBurstM` and the
// fireball above it inside the solver domain [0, 2 * half]. Sizing the cube
// only from the fireball radius used to clamp high bursts to the domain
// ceiling, so the resolved cloud sat far below the analytic shock front.
inline double SolverHalfExtentForBurst(double heightOfBurstM, double fireballRadiusM) {
    const double fireball = std::max(fireballRadiusM, 0.1);
    // 0.55 h keeps the burst and its fireball below the absorbing sponge, which
    // occupies the outer 6 % of the domain top.
    return std::max(3.0 * fireball,
                    0.55 * std::max(heightOfBurstM, 0.0) + 1.5 * fireball);
}

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
    // Radius of the upper plume only (above the base surge), used to place the
    // cap closure ring.
    float plumeCapRadius = 0.0f;
    float plumeDomeFraction = 0.0f;
    bool fieldBoundsValid = false;
};

// GPU passes the solver can time with timestamp queries.
enum class SolverPass : int {
    SweepX = 0,
    SweepY,
    SweepZ,   // the fused source alternates between the three sweeps
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

} // namespace ex::gfx
