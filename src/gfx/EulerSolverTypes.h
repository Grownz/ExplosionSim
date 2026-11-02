#pragma once

#include <algorithm>
#include <cmath>
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

// Wall-clock budget for the high-fidelity solver steps of one interactive
// frame, on top of the step cap (solverStepsPerFrame). The step cap assumes a
// fixed cost per step, but right after the detonation the GPU is usually still
// at its idle clock and the same steps take several times longer, which
// stalled the first frame for a few hundred milliseconds. The frame loop
// submits the steps in batches, waits for each batch that another batch
// follows and asks the budget how many more steps fit. The cost per step is
// re-measured after every batch, so the frame follows the GPU clock while it
// ramps up. A budget of 0 disables the limit.
class SolverStepBudget {
public:
    // Steps of the first batch of a frame, before the cost per step is known.
    // A single step: at the idle GPU and memory clock one step can take more
    // than 15 ms instead of ~0.6 ms (RTX 3090, 128³).
    static constexpr int kFirstBatch = 1;

    explicit SolverStepBudget(double budgetMs) : budgetMs_(std::max(budgetMs, 0.0)) {}

    bool Enabled() const { return budgetMs_ > 0.0; }

    // Steps allowed for the next batch, at most `wanted`; 0 when the budget is
    // spent. The first batch always runs, so every frame makes progress.
    int Limit(int wanted) const {
        if (wanted <= 0 || !Enabled()) {
            return std::max(wanted, 0);
        }
        if (batches_ == 0) {
            return std::min(wanted, kFirstBatch);
        }
        const double remaining = budgetMs_ - elapsedMs_;
        if (remaining <= 0.0) {
            return 0;
        }
        if (msPerStep_ <= 0.0) {
            return wanted;
        }
        const double fit = std::floor(remaining / msPerStep_);
        return fit >= double(wanted) ? wanted : int(fit);
    }

    // Records a finished batch and its wall time, including the wait for the
    // GPU.
    void Record(int steps, double milliseconds) {
        milliseconds = std::max(milliseconds, 0.0);
        elapsedMs_ += milliseconds;
        if (steps > 0) {
            msPerStep_ = milliseconds / double(steps);
        }
        ++batches_;
    }

    double ElapsedMs() const { return elapsedMs_; }

private:
    double budgetMs_ = 0.0;
    double elapsedMs_ = 0.0;
    double msPerStep_ = 0.0;
    int batches_ = 0;
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
