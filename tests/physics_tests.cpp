// Lightweight regression tests for the physics core and the thread pool.
// No external test framework is required.

#include "core/ThreadPool.h"
#include "sim/BlastModel.h"
#include "sim/Simulation.h"
#include "sim/VolumeBaker.h"

#include <DirectXPackedVector.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::printf("FAILED: %s (%s:%d)\n", expression, file, line);
        ++g_failures;
    }
}

#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)
#define CHECK_NEAR(value, expected, tolerance)                                              \
    Check(std::abs(double(value) - double(expected)) <= double(tolerance), #value " ~ " #expected, \
          __FILE__, __LINE__)

using ex::sim::BlastModel;
using ex::sim::Charge;
using ex::sim::Environment;

void TestChargeScaling() {
    Charge charge{};
    charge.tntMassKg = 1000.0;
    CHECK_NEAR(charge.EnergyJ(), 4.184e9, 1.0);

    const double cbrt = charge.CbrtMass();
    CHECK_NEAR(cbrt, 10.0, 1e-9);
    CHECK_NEAR(charge.ScaledDistance(25.0), 2.5, 1e-9);

    charge.heightOfBurstM = 15.0;
    CHECK(charge.RadiusM() > 0.1 && charge.RadiusM() < 1.0);
}

void TestAtmosphere() {
    Environment environment{};
    CHECK_NEAR(environment.AirDensity(), 1.225, 0.01);
    CHECK_NEAR(environment.SoundSpeed(), 340.3, 1.0);
    CHECK_NEAR(environment.SpecificHeatCp(), 1004.7, 3.0);

    Environment dry{};
    dry.relativeHumidity = 0.0;
    Environment wet{};
    wet.relativeHumidity = 1.0;
    CHECK(wet.AirDensity() < dry.AirDensity());
    CHECK(wet.SoundSpeed() > dry.SoundSpeed());
    CHECK(wet.VaporPressurePa() > 1000.0);
    CHECK(wet.SpecificHumidity() > 0.0);
}

void TestAltitudeScaling() {
    Charge charge{};
    charge.tntMassKg = 1.0;
    charge.heightOfBurstM = 1.0;

    Environment seaLevel{};
    seaLevel.pressurePa = 101325.0;
    Environment altitude{};
    altitude.pressurePa = 50000.0;

    BlastModel sea;
    sea.Configure(charge, seaLevel);
    BlastModel high;
    high.Configure(charge, altitude);

    // Sachs scaling: a charge at altitude produces a larger fireball; at a
    // fixed physical range the absolute overpressure is lower because the
    // ambient pressure is lower.
    CHECK(high.FireballMaxRadiusM() > sea.FireballMaxRadiusM());
    CHECK(high.PeakOverpressureAtDistance(100.0) < sea.PeakOverpressureAtDistance(100.0));
    CHECK(high.State().shockSpeed > 0.0);
}

void TestOverpressureFit() {
    // Brode's far-field fit at Z = 1 m/kg^(1/3): 8.26 bar.
    CHECK_NEAR(BlastModel::PeakOverpressurePa(1.0, 101325.0), 826100.0, 3000.0);

    double previous = 1.0e30;
    for (double z = 0.2; z < 60.0; z *= 1.15) {
        const double pressure = BlastModel::PeakOverpressurePa(z, 101325.0);
        CHECK(pressure > 0.0);
        if (z > 0.35) {
            CHECK(pressure < previous);
        }
        previous = pressure;
    }
}

void TestBlastEvolution() {
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 20000.0;
    charge.heightOfBurstM = 30.0;

    BlastModel model;
    model.Configure(charge, environment);

    const double initialRadius = model.State().shockRadius;
    CHECK(initialRadius > 0.0);
    CHECK(model.State().fireballRadius > 0.0);

    double previousRadius = initialRadius;
    for (int step = 0; step < 600; ++step) {
        model.Advance(1.0 / 120.0);
        CHECK(model.State().shockRadius >= previousRadius);
        previousRadius = model.State().shockRadius;
    }

    const auto& state = model.State();
    CHECK(state.time > 4.9 && state.time < 5.1);
    CHECK(state.shockRadius > 100.0);
    CHECK(state.shockSpeed > environment.SoundSpeed() * 0.5);
    CHECK(state.fireballRadius > 5.0);
    CHECK(state.fireballRadius < 100.0);
    CHECK(state.cloudCenterY > charge.heightOfBurstM);

    // Arrival times must be finite and ordered.
    const double arrival100 = model.ArrivalTimeS(100.0);
    const double arrival300 = model.ArrivalTimeS(300.0);
    CHECK(arrival100 > 0.0);
    CHECK(arrival300 > arrival100);
    CHECK(model.ArrivalTimeS(1e7) < 0.0);

    // Inverse damage radius must be consistent with the forward model.
    const double target = 34474.0; // 5 psi
    const double radius = model.RadiusForOverpressure(target);
    CHECK(radius > 10.0);
    CHECK_NEAR(model.PeakOverpressureAtDistance(radius), target, target * 0.02);

    // Thermal fluence inverse.
    const double fluence = 80000.0;
    const double thermalRadius = model.RadiusForThermalFluence(fluence);
    CHECK_NEAR(model.ThermalFluenceJm2(thermalRadius), fluence, 1.0);

    // Friedlander waveform: peak at arrival, decaying afterwards.
    const double peak = model.PeakOverpressureAtDistance(100.0);
    CHECK_NEAR(model.OverpressureAtTime(100.0, arrival100), peak, peak * 0.05);
    CHECK(model.OverpressureAtTime(100.0, arrival100 + 10.0) < peak * 0.1);
}

void TestThreadPool() {
    ex::core::ThreadPool pool(3);

    std::atomic<long long> sum{0};
    pool.ParallelFor(10000, 137, [&sum](int begin, int end) {
        long long local = 0;
        for (int i = begin; i < end; ++i) {
            local += i;
        }
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    CHECK(sum.load() == 10000LL * 9999LL / 2);

    std::atomic<int> asyncCounter{0};
    std::atomic<bool> done{false};
    pool.ParallelForAsync(
        1000, 64,
        [&asyncCounter](int begin, int end) { asyncCounter.fetch_add(end - begin); },
        [&done] { done.store(true); });

    for (int i = 0; i < 5000 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(done.load());
    CHECK(asyncCounter.load() == 1000);
}

void TestVolumeBaker() {
    ex::core::ThreadPool pool(3);
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 20000.0;
    charge.heightOfBurstM = 30.0;

    BlastModel model;
    model.Configure(charge, environment);
    for (int i = 0; i < 120; ++i) {
        model.Advance(1.0 / 120.0);
    }

    ex::sim::VolumeBaker baker;
    baker.Initialize(&pool, 32);
    baker.BakeNow(model);
    baker.Poll();

    CHECK(baker.HasData());
    const auto& frame = baker.Current();
    CHECK(frame.resolution == 32);
    CHECK(!frame.rgba.empty());
    CHECK(frame.bounds.max.y > 0.0f);
    CHECK(frame.bounds.max.x > frame.bounds.min.x);

    int occupied = 0;
    for (size_t i = 0; i < frame.rgba.size(); i += size_t(frame.bytesPerVoxel)) {
        float density = 0.0f;
        if (frame.halfFloat) {
            uint16_t bits = 0;
            std::memcpy(&bits, &frame.rgba[i], sizeof(bits));
            auto half = *reinterpret_cast<DirectX::PackedVector::HALF*>(&bits);
            density = DirectX::PackedVector::XMConvertHalfToFloat(half);
        } else {
            density = float(frame.rgba[i]) / 255.0f;
        }
        CHECK(std::isfinite(density));
        if (density > 0.01f) {
            ++occupied;
        }
    }
    CHECK(occupied > 20);
}

void TestBrodeCrossover() {
    // The near/far-field blend must stay monotonic across the crossover so the
    // inverse bisection cannot converge to a spurious root.
    double previous = 1.0e30;
    for (double z = 0.5; z <= 1.5; z += 0.002) {
        const double pressure = BlastModel::PeakOverpressurePa(z, 101325.0);
        CHECK(pressure < previous);
        previous = pressure;
    }

    Charge charge{};
    charge.tntMassKg = 20000.0;
    charge.heightOfBurstM = 30.0;
    Environment environment{};
    BlastModel model;
    model.Configure(charge, environment);

    // 10 bar sits directly on the smoothed crossover.
    const double target = 1.0e6;
    const double radius = model.RadiusForOverpressure(target);
    CHECK(radius > 0.0);
    CHECK_NEAR(model.PeakOverpressureAtDistance(radius), target, target * 0.02);
}

void TestSmallChargeScaling() {
    // The reference scenario: 200 g of TNT at 0.4 m height.
    Charge charge{};
    charge.tntMassKg = 0.2;
    charge.heightOfBurstM = 0.4;

    Environment environment{};
    BlastModel model;
    model.Configure(charge, environment);

    CHECK(model.FireballMaxRadiusM() > 0.4);
    CHECK(model.FireballMaxRadiusM() < 1.2);
    CHECK(model.State().shockRadius > 0.0);

    const double psi5 = model.RadiusForOverpressure(34474.0);
    CHECK(psi5 > 1.0 && psi5 < 8.0);
}

void TestVolumeBakerAutoScale() {
    ex::core::ThreadPool pool(2);
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 1.0;
    charge.heightOfBurstM = 1.0;

    BlastModel model;
    model.Configure(charge, environment);

    ex::sim::VolumeBaker baker;
    baker.Initialize(&pool, 32);
    baker.SetAutoScale(true);

    for (int i = 0; i < 30; ++i) {
        model.Advance(1.0 / 120.0);
    }
    baker.BakeNow(model);
    baker.Poll();
    const ex::sim::VolumeBounds first = baker.Current().bounds;
    baker.MarkUploaded(baker.Version());

    for (int i = 0; i < 240; ++i) {
        model.Advance(1.0 / 120.0);
    }
    baker.BakeNow(model);
    baker.Poll();
    const ex::sim::VolumeBounds second = baker.Current().bounds;

    // The auto-scale box only grows and must contain the rising cloud.
    CHECK(second.max.x >= first.max.x);
    CHECK(second.max.y >= first.max.y);
    CHECK(second.min.x <= first.min.x);
    const auto& state = model.State();
    CHECK(second.max.y >= state.cloudCenterY + state.cloudRadius);
    CHECK(second.max.x >= state.cloudRadius);
}

void TestVolumeBakeDeterminism() {
    ex::core::ThreadPool pool(2);
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 1.0;
    charge.heightOfBurstM = 1.0;

    BlastModel model;
    model.Configure(charge, environment);
    for (int i = 0; i < 60; ++i) {
        model.Advance(1.0 / 120.0);
    }

    ex::sim::VolumeBaker baker;
    baker.Initialize(&pool, 24);
    baker.BakeNow(model);
    baker.Poll();
    const std::vector<uint8_t> first = baker.Current().rgba;
    baker.MarkUploaded(baker.Version());

    baker.BakeNow(model);
    baker.Poll();
    const std::vector<uint8_t>& second = baker.Current().rgba;

    CHECK(!first.empty());
    CHECK(first.size() == second.size());
    CHECK(first == second);
}

void TestFuseAndDetonation() {
    ex::core::ThreadPool pool(2);

    ex::sim::SimulationSettings settings{};
    settings.chargeMassKg = 0.2;
    settings.heightOfBurstM = 0.4;
    settings.fuseSeconds = 1.0;
    settings.loop = false;
    settings.volumeResolution = 32;
    settings.particleBudget = 4000;

    ex::sim::Simulation simulation;
    CHECK(simulation.Initialize(settings, pool));
    CHECK(simulation.IsArmed());
    CHECK_NEAR(simulation.FuseRemaining(), 1.0, 1e-9);

    for (int i = 0; i < 5; ++i) {
        simulation.Update(0.1f);
    }
    CHECK(simulation.IsArmed());
    CHECK_NEAR(simulation.FuseRemaining(), 0.5, 1e-5);

    for (int i = 0; i < 10; ++i) {
        simulation.Update(0.1f);
    }
    CHECK(!simulation.IsArmed());
    CHECK(simulation.Time() > 0.0);
    CHECK(simulation.Particles().AliveCount() > 0);

    simulation.Shutdown();
}

} // namespace

int main() {
    TestChargeScaling();
    TestAtmosphere();
    TestAltitudeScaling();
    TestOverpressureFit();
    TestBrodeCrossover();
    TestBlastEvolution();
    TestSmallChargeScaling();
    TestThreadPool();
    TestVolumeBaker();
    TestVolumeBakerAutoScale();
    TestVolumeBakeDeterminism();
    TestFuseAndDetonation();

    if (g_failures == 0) {
        std::printf("All physics tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
