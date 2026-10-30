// Portable smoke tests for the platform-independent simulation core. These run
// on Windows and Linux and cover the pieces that must not depend on D3D11 or
// Win32: blast scaling, procedural scene generation and the CPU volume bake.

#include "core/ThreadPool.h"
#include "scene/SceneTypes.h"
#include "sim/BlastModel.h"
#include "sim/VolumeBaker.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #condition);                                         \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

void TestBlastModel() {
    ex::sim::Charge charge{};
    charge.tntMassKg = 20000.0;
    charge.heightOfBurstM = 30.0;
    ex::sim::Environment environment{};

    ex::sim::BlastModel model;
    model.Configure(charge, environment);

    const double nearPressure = model.PeakOverpressureAtDistance(100.0);
    const double farPressure = model.PeakOverpressureAtDistance(1000.0);
    CHECK(nearPressure > 0.0);
    CHECK(farPressure > 0.0);
    CHECK(nearPressure > farPressure);
    CHECK(model.FireballMaxRadiusM() > 0.0);
    CHECK(model.RadiusForOverpressure(1000.0) > 0.0);

    model.Reset();
    for (int step = 0; step < 120; ++step) {
        model.Advance(1.0 / 60.0);
    }
    CHECK(model.State().shockRadius > 0.0);
    CHECK(model.State().shockSpeed > 0.0);
}

void TestSceneDeterminism() {
    ex::sim::BlastModel blast;
    blast.Configure({}, {});

    ex::scene::SceneSettings settings{};
    settings.seed = 42;
    settings.density = 0.25f;
    settings.quality = 0;
    settings.voronoiChunks = true;

    ex::scene::Scene first;
    ex::scene::Scene second;
    first.Generate(settings, blast);
    second.Generate(settings, blast);

    CHECK(!first.Empty());
    CHECK(first.GenerationHash() == second.GenerationHash());
    CHECK(first.Objects().size() == second.Objects().size());

    int chunks = 0;
    for (const ex::scene::SceneObject& object : first.Objects()) {
        chunks += int(object.chunks.size());
    }
    CHECK(chunks > 0);
}

void TestVolumeBake() {
    ex::core::ThreadPool pool(2);
    ex::sim::BlastModel blast;
    blast.Configure({}, {});
    blast.Reset();
    for (int step = 0; step < 30; ++step) {
        blast.Advance(1.0 / 60.0);
    }

    ex::sim::VolumeBaker baker;
    baker.Initialize(&pool, 16);
    baker.BakeNow(blast);
    CHECK(baker.HasData());
    CHECK(baker.Current().resolution == 16);
    CHECK(!baker.Current().empty());
    CHECK(baker.Current().halfFloat);
}

} // namespace

int main() {
    TestBlastModel();
    TestSceneDeterminism();
    TestVolumeBake();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d core test(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All core tests passed.\n");
    return 0;
}
