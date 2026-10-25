// Lightweight regression tests for the physics core and the thread pool.
// No external test framework is required.

#include "core/ThreadPool.h"
#include "app/Settings.h"
#include "gfx/Shader.h"
#include "scene/PhysicsWorld.h"
#include "scene/SceneTypes.h"
#include "sim/BlastModel.h"
#include "sim/Simulation.h"
#include "sim/VolumeBaker.h"

#include <DirectXPackedVector.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

// The settings serializer only needs this symbol for DefaultSettingsPath; the
// round-trip test passes an explicit path.
namespace ex::gfx {
std::wstring ResolveAssetPath(const std::wstring&) {
    return {};
}
} // namespace ex::gfx

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

void TestSceneGeneration() {
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 0.2;
    charge.heightOfBurstM = 0.4;
    BlastModel model;
    model.Configure(charge, environment);

    ex::scene::SceneSettings settings{};
    settings.seed = 42;
    settings.density = 1.0f;

    ex::scene::Scene first;
    ex::scene::Scene second;
    first.Generate(settings, model);
    second.Generate(settings, model);

    CHECK(!first.Empty());
    CHECK(first.GenerationHash() == second.GenerationHash());
    CHECK(first.Objects().size() == second.Objects().size());
    CHECK(first.Objects().size() <= 800);
    CHECK(first.Extent() > 0.0f);

    settings.seed = 43;
    second.Generate(settings, model);
    CHECK(first.GenerationHash() != second.GenerationHash());

    for (const ex::scene::SceneObject& object : first.Objects()) {
        CHECK(object.position.y == 0.0f);
        CHECK(object.distanceToBurst >= 0.0f);
        CHECK(object.arrivalTime >= 0.0f);
        CHECK(object.peakOverpressurePa >= 0.0f);
        CHECK(object.thermalFluenceJm2 >= 0.0f);
        CHECK(object.meshVariant >= 0);
    }

    settings.enabled = false;
    second.Generate(settings, model);
    CHECK(second.Empty());
    CHECK(second.GenerationHash() == 0);
}

void TestPhysicsWorld() {
    ex::scene::PhysicsWorld world;
    ex::scene::RigidBody body;
    body.position = DirectX::XMFLOAT3(0.0f, 10.0f, 0.0f);
    body.halfExtents = DirectX::XMFLOAT3(0.5f, 0.5f, 0.5f);
    body.boundingRadius = 0.5f;
    body.mass = 1.0f;
    body.invMass = 1.0f;
    CHECK(world.AddBody(body) == 0);

    for (int i = 0; i < 600; ++i) {
        world.Step(1.0f / 120.0f);
    }

    CHECK(world.Bodies().size() == 1);
    const ex::scene::RigidBody& settled = world.Bodies()[0];
    CHECK(std::isfinite(settled.position.y));
    CHECK(settled.position.y >= 0.5f - 1e-3f);
    CHECK(settled.position.y < 1.0f);
    CHECK(settled.sleeping);
}

void TestSceneDamage() {
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 30.0;
    charge.heightOfBurstM = 0.4;
    BlastModel model;
    model.Configure(charge, environment);

    ex::scene::SceneSettings settings{};
    settings.seed = 7;
    settings.density = 1.0f;
    ex::scene::Scene scene;
    scene.Generate(settings, model);
    CHECK(!scene.Empty());

    scene.Update(model, 10.0f);

    int damaged = 0;
    int collapsed = 0;
    for (const ex::scene::SceneObject& object : scene.Objects()) {
        CHECK(object.resolved);
        if (object.damage != ex::scene::DamageState::Intact) {
            ++damaged;
        }
        if (object.collapsed) {
            ++collapsed;
        }
        if (object.damage == ex::scene::DamageState::Destroyed) {
            CHECK(object.damageAmount > 0.9f);
        }
    }

    CHECK(damaged > 0);
    CHECK(collapsed > 0);

    const std::vector<ex::scene::SceneDebrisSpawn> spawns = scene.ConsumeDebrisSpawns();
    CHECK(!spawns.empty());

    scene.Update(model, 0.1f);
    CHECK(scene.ConsumeDebrisSpawns().empty());

    // A small charge must leave the outer town intact (P-I thresholds).
    Charge light{};
    light.tntMassKg = 3.0;
    light.heightOfBurstM = 0.4;
    BlastModel lightModel;
    lightModel.Configure(light, environment);
    ex::scene::Scene lightScene;
    lightScene.Generate(settings, lightModel);
    CHECK(!lightScene.Empty());
    lightScene.Update(lightModel, 10.0f);
    int intact = 0;
    for (const ex::scene::SceneObject& object : lightScene.Objects()) {
        if (object.damage == ex::scene::DamageState::Intact) {
            ++intact;
        }
    }
    CHECK(intact > 0);
}

void TestSceneFire() {
    Environment environment{};
    Charge charge{};
    charge.tntMassKg = 1000.0;
    charge.heightOfBurstM = 0.4;
    BlastModel model;
    model.Configure(charge, environment);

    ex::scene::SceneSettings settings{};
    settings.seed = 7;
    settings.density = 1.0f;
    ex::scene::Scene scene;
    scene.Generate(settings, model);
    CHECK(!scene.Empty());

    int shielded = 0;
    for (const ex::scene::SceneObject& object : scene.Objects()) {
        if (object.occluders > 0) {
            ++shielded;
        }
    }
    CHECK(shielded > 0);

    scene.Update(model, 10.0f);
    CHECK(!scene.ConsumeFireSpawns().empty());
}

void TestSettingsRoundTrip() {
    ex::app::AppSettings original{};
    original.simulation.chargeMassKg = 12.5;
    original.simulation.heightOfBurstM = 3.25;
    original.simulation.ambientTemperatureK = 300.15;
    original.simulation.ambientPressurePa = 95000.0;
    original.simulation.relativeHumidity = 0.35;
    original.simulation.windSpeedMs = 4.5;
    original.simulation.windDirectionDeg = 123.0;
    original.simulation.timeScale = 0.25;
    original.simulation.fuseSeconds = 1.5;
    original.simulation.mode = ex::sim::SimulationMode::HighFidelity;
    original.simulation.solverResolution = 96;
    original.simulation.solverStepsPerFrame = 12;
    original.simulation.solverAutoScale = false;
    original.simulation.loop = true;
    original.simulation.particleBudget = 12345;
    original.simulation.volumeResolution = 96;
    original.simulation.bakeVolume = false;
    original.simulation.autoScaleVolume = false;
    original.simulation.scene.enabled = true;
    original.simulation.scene.seed = 4242;
    original.simulation.scene.density = 1.5f;
    original.simulation.scene.buildings = false;
    original.simulation.scene.vehicles = true;
    original.simulation.scene.vegetation = false;
    original.simulation.scene.quality = 2;

    original.render.raymarchSteps = 77;
    original.render.exposure = 1.25f;
    original.render.bloomStrength = 0.42f;
    original.render.enableVolume = false;
    original.render.enableParticles = false;
    original.render.enableGround = false;
    original.render.enableBloom = false;
    original.render.showGrid = false;
    original.render.sunAzimuthDeg = 111.0f;
    original.render.sunElevationDeg = 27.0f;
    original.render.particleScale = 1.75f;

    original.paused = true;
    original.showUi = false;
    original.observerDistanceM = 42.0;

    const std::wstring path = L"test_settings_roundtrip.ini";
    ex::app::SaveSettings(original, path);

    ex::app::AppSettings loaded{};
    ex::app::LoadSettings(loaded, path);
    _wremove(path.c_str());

    CHECK_NEAR(loaded.simulation.chargeMassKg, 12.5, 1e-4);
    CHECK_NEAR(loaded.simulation.heightOfBurstM, 3.25, 1e-4);
    CHECK_NEAR(loaded.simulation.ambientTemperatureK, 300.15, 1e-4);
    CHECK_NEAR(loaded.simulation.ambientPressurePa, 95000.0, 1e-4);
    CHECK_NEAR(loaded.simulation.relativeHumidity, 0.35, 1e-4);
    CHECK_NEAR(loaded.simulation.windSpeedMs, 4.5, 1e-4);
    CHECK_NEAR(loaded.simulation.windDirectionDeg, 123.0, 1e-4);
    CHECK_NEAR(loaded.simulation.timeScale, 0.25, 1e-4);
    CHECK_NEAR(loaded.simulation.fuseSeconds, 1.5, 1e-4);
    CHECK(loaded.simulation.mode == ex::sim::SimulationMode::HighFidelity);
    CHECK(loaded.simulation.solverResolution == 96);
    CHECK(loaded.simulation.solverStepsPerFrame == 12);
    CHECK(!loaded.simulation.solverAutoScale);
    CHECK(loaded.simulation.loop);
    CHECK(loaded.simulation.particleBudget == 12345);
    CHECK(loaded.simulation.volumeResolution == 96);
    CHECK(!loaded.simulation.bakeVolume);
    CHECK(!loaded.simulation.autoScaleVolume);
    CHECK(loaded.simulation.scene.enabled);
    CHECK(loaded.simulation.scene.seed == 4242u);
    CHECK_NEAR(loaded.simulation.scene.density, 1.5f, 1e-4);
    CHECK(!loaded.simulation.scene.buildings);
    CHECK(loaded.simulation.scene.vehicles);
    CHECK(!loaded.simulation.scene.vegetation);
    CHECK(loaded.simulation.scene.quality == 2);

    CHECK(loaded.render.raymarchSteps == 77);
    CHECK_NEAR(loaded.render.exposure, 1.25f, 1e-4);
    CHECK_NEAR(loaded.render.bloomStrength, 0.42f, 1e-4);
    CHECK(!loaded.render.enableVolume);
    CHECK(!loaded.render.enableParticles);
    CHECK(!loaded.render.enableGround);
    CHECK(!loaded.render.enableBloom);
    CHECK(!loaded.render.showGrid);
    CHECK_NEAR(loaded.render.sunAzimuthDeg, 111.0f, 1e-4);
    CHECK_NEAR(loaded.render.sunElevationDeg, 27.0f, 1e-4);
    CHECK_NEAR(loaded.render.particleScale, 1.75f, 1e-4);

    CHECK(loaded.paused);
    CHECK(!loaded.showUi);
    CHECK_NEAR(loaded.observerDistanceM, 42.0, 1e-4);
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
    TestSceneGeneration();
    TestSceneDamage();
    TestSceneFire();
    TestPhysicsWorld();
    TestSettingsRoundTrip();
    TestFuseAndDetonation();

    if (g_failures == 0) {
        std::printf("All physics tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
