// Portable smoke tests for the platform-independent simulation core. These run
// on Windows and Linux and cover the pieces that must not depend on D3D11 or
// Win32: blast scaling, procedural scene generation and the CPU volume bake.

#include "core/ThreadPool.h"
#include "scene/SceneTypes.h"
#include "sim/BlastModel.h"
#include "sim/FlowBodies.h"
#include "sim/Obstacles.h"
#include "sim/VolumeBaker.h"

#include <DirectXPackedVector.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

void TestObstacleVoxelization() {
    ex::sim::VolumeBounds bounds{};
    bounds.min = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    bounds.max = DirectX::XMFLOAT3(8.0f, 8.0f, 8.0f);

    const auto at = [](const std::vector<float>& mask, int x, int y, int z) {
        return mask[(size_t(z) * 8u + size_t(y)) * 8u + size_t(x)];
    };
    const auto countSolid = [](const std::vector<float>& mask) {
        int count = 0;
        for (float value : mask) {
            count += value > 0.5f ? 1 : 0;
        }
        return count;
    };

    std::vector<ex::sim::ObstacleBox> boxes(1);
    boxes[0].center = DirectX::XMFLOAT3(4.0f, 4.0f, 4.0f);
    boxes[0].halfExtents = DirectX::XMFLOAT3(2.0f, 2.0f, 2.0f);

    const std::vector<float> mask =
        ex::sim::VoxelizeObstacles(boxes, bounds, 8);
    CHECK(mask.size() == 512);
    // Half extents plus half a cell give six solid cells per axis.
    CHECK(countSolid(mask) == 216);
    CHECK(at(mask, 1, 1, 1) > 0.5f);
    CHECK(at(mask, 4, 4, 4) > 0.5f);
    CHECK(at(mask, 6, 6, 6) > 0.5f);
    CHECK(at(mask, 0, 0, 0) < 0.5f);
    CHECK(at(mask, 7, 7, 7) < 0.5f);

    const std::vector<float> empty =
        ex::sim::VoxelizeObstacles({}, bounds, 8);
    CHECK(empty.size() == 512);
    CHECK(countSolid(empty) == 0);

    // A long box rotated by 90 degrees about Y swaps its X and Z extents.
    boxes[0].halfExtents = DirectX::XMFLOAT3(3.0f, 1.0f, 1.0f);
    const std::vector<float> unrotated =
        ex::sim::VoxelizeObstacles(boxes, bounds, 8);
    CHECK(at(unrotated, 0, 4, 4) > 0.5f);
    CHECK(at(unrotated, 4, 4, 0) < 0.5f);

    DirectX::XMStoreFloat4(
        &boxes[0].orientation,
        DirectX::XMQuaternionRotationRollPitchYaw(
            0.0f, DirectX::XMConvertToRadians(90.0f), 0.0f));
    const std::vector<float> rotated =
        ex::sim::VoxelizeObstacles(boxes, bounds, 8);
    CHECK(at(rotated, 0, 4, 4) < 0.5f);
    CHECK(at(rotated, 4, 4, 0) > 0.5f);
}

void TestVolumeObstacleCache() {
    ex::sim::BlastModel blast;
    blast.Configure({}, {});

    ex::scene::SceneSettings settings{};
    settings.seed = 7;
    settings.density = 0.25f;
    settings.quality = 0;

    ex::scene::Scene scene;
    scene.Generate(settings, blast);

    const ex::scene::SceneObject* building = nullptr;
    for (const ex::scene::SceneObject& object : scene.Objects()) {
        if (object.type == ex::scene::SceneObjectType::Building &&
            !object.collapsed &&
            object.damage != ex::scene::DamageState::Destroyed) {
            building = &object;
            break;
        }
    }
    CHECK(building != nullptr);
    if (!building) {
        return;
    }

    ex::sim::VolumeBounds bounds{};
    bounds.min = DirectX::XMFLOAT3(building->position.x - building->size.x,
                                   building->position.y - building->size.y,
                                   building->position.z - building->size.z);
    bounds.max = DirectX::XMFLOAT3(building->position.x + building->size.x,
                                   building->position.y + 2.0f * building->size.y,
                                   building->position.z + building->size.z);

    ex::sim::VolumeObstacleMask cache;
    const auto first = cache.Update(scene, bounds, 16);
    CHECK(first != nullptr);
    CHECK(cache.Resolution() == 16);
    const uint64_t firstVersion = cache.Version();
    CHECK(firstVersion > 0);
    int solids = 0;
    for (float value : *first) {
        solids += value > 0.5f ? 1 : 0;
    }
    CHECK(solids > 0);

    // An unchanged scene and box reuse the same mask and version.
    const auto again = cache.Update(scene, bounds, 16);
    CHECK(again.get() == first.get());
    CHECK(cache.Version() == firstVersion);

    // An unchanged scene reuses the mask even when the bake box moved a little
    // (for example with the downwind drift); only leaving the covered box or a
    // scene change rebuilds it.
    ex::sim::VolumeBounds inside = bounds;
    inside.min.x += 0.5f;
    inside.max.x -= 0.5f;
    const auto drifted = cache.Update(scene, inside, 16);
    CHECK(drifted.get() == first.get());
    CHECK(cache.Version() == firstVersion);

    // Moving the box outside the covered box rebuilds the mask.
    bounds.max.x += 2.0f;
    const auto moved = cache.Update(scene, bounds, 16);
    CHECK(moved.get() != first.get());
    CHECK(cache.Version() == firstVersion + 1);
}

void TestObstacleVelocityMask() {
    ex::sim::VolumeBounds bounds{};
    bounds.min = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    bounds.max = DirectX::XMFLOAT3(8.0f, 8.0f, 8.0f);

    std::vector<ex::sim::ObstacleBox> boxes(1);
    boxes[0].center = DirectX::XMFLOAT3(4.0f, 4.0f, 4.0f);
    boxes[0].halfExtents = DirectX::XMFLOAT3(2.0f, 2.0f, 2.0f);
    boxes[0].velocity = DirectX::XMFLOAT3(1.0f, 2.0f, 3.0f);

    const std::vector<uint16_t> mask =
        ex::sim::VoxelizeObstacleVelocityMask(boxes, bounds, 8);
    CHECK(mask.size() == 512u * 4u);
    const auto channel = [&](int x, int y, int z, int c) {
        return mask[((size_t(z) * 8u + size_t(y)) * 8u + size_t(x)) * 4u +
                    size_t(c)];
    };
    const auto decode = [](uint16_t bits) {
        DirectX::PackedVector::HALF half = 0;
        std::memcpy(&half, &bits, sizeof(half));
        return DirectX::PackedVector::XMConvertHalfToFloat(half);
    };
    CHECK(decode(channel(4, 4, 4, 3)) > 0.5f);
    CHECK(std::abs(decode(channel(4, 4, 4, 0)) - 1.0f) < 1e-3f);
    CHECK(std::abs(decode(channel(4, 4, 4, 1)) - 2.0f) < 1e-3f);
    CHECK(std::abs(decode(channel(4, 4, 4, 2)) - 3.0f) < 1e-3f);
    CHECK(decode(channel(0, 0, 0, 3)) < 0.5f);
    CHECK(decode(channel(0, 0, 0, 0)) == 0.0f);

    const std::vector<uint16_t> empty =
        ex::sim::VoxelizeObstacleVelocityMask({}, bounds, 8);
    CHECK(empty.size() == 512u * 4u);
    for (uint16_t value : empty) {
        CHECK(value == 0);
    }
}

void TestPhysicsObstacles() {
    ex::scene::PhysicsWorld physics;

    ex::scene::RigidBody body;
    body.position = DirectX::XMFLOAT3(1.0f, 2.0f, 3.0f);
    body.velocity = DirectX::XMFLOAT3(4.0f, 5.0f, 6.0f);
    body.halfExtents = DirectX::XMFLOAT3(0.5f, 0.5f, 0.5f);
    body.mass = 10.0f;
    CHECK(physics.AddBody(body) == 0);

    ex::sim::VolumeBounds inside{};
    inside.min = DirectX::XMFLOAT3(-5.0f, -5.0f, -5.0f);
    inside.max = DirectX::XMFLOAT3(5.0f, 5.0f, 5.0f);
    std::vector<ex::sim::ObstacleBox> boxes =
        ex::sim::PhysicsObstacles(physics, inside);
    CHECK(boxes.size() == 1);
    CHECK(boxes[0].center.x == 1.0f && boxes[0].center.y == 2.0f &&
          boxes[0].center.z == 3.0f);
    CHECK(boxes[0].velocity.x == 4.0f && boxes[0].velocity.y == 5.0f &&
          boxes[0].velocity.z == 6.0f);
    CHECK(boxes[0].halfExtents.x == 0.5f);

    // Bodies outside the box cannot affect the flow.
    ex::sim::VolumeBounds outside{};
    outside.min = DirectX::XMFLOAT3(100.0f, 100.0f, 100.0f);
    outside.max = DirectX::XMFLOAT3(110.0f, 110.0f, 110.0f);
    CHECK(ex::sim::PhysicsObstacles(physics, outside).empty());

    // Static structure keeps its place but has no wall velocity.
    ex::scene::RigidBody wall;
    wall.position = DirectX::XMFLOAT3(2.0f, 2.0f, 2.0f);
    wall.velocity = DirectX::XMFLOAT3(9.0f, 9.0f, 9.0f);
    wall.halfExtents = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
    wall.isStatic = true;
    CHECK(physics.AddBody(wall) == 1);
    boxes = ex::sim::PhysicsObstacles(physics, inside);
    CHECK(boxes.size() == 2);
    CHECK(boxes[1].velocity.x == 0.0f && boxes[1].velocity.y == 0.0f &&
          boxes[1].velocity.z == 0.0f);
}

void TestBodyDrag() {
    ex::sim::FlowBody box;
    box.halfExtents = DirectX::XMFLOAT3(0.5f, 0.5f, 0.5f);
    const float area = ex::sim::ProjectedArea(
        box, DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f));
    CHECK(std::abs(area - 1.0f) < 1e-4f);
    const float diagonal = ex::sim::ProjectedArea(
        box, DirectX::XMFLOAT3(0.57735f, 0.57735f, 0.57735f));
    CHECK(diagonal > area);

    ex::scene::PhysicsWorld physics;
    ex::scene::RigidBody rigid;
    rigid.position = DirectX::XMFLOAT3(0.0f, 5.0f, 0.0f);
    rigid.halfExtents = DirectX::XMFLOAT3(0.5f, 0.5f, 0.5f);
    rigid.mass = 10.0f;
    const int index = physics.AddBody(rigid);
    CHECK(index == 0);

    ex::sim::FlowBody body;
    body.position = rigid.position;
    body.halfExtents = rigid.halfExtents;
    body.physicsIndex = index;
    ex::sim::BodyFlowSample sample;
    sample.density = 1.2f;
    sample.velocity = DirectX::XMFLOAT3(10.0f, 0.0f, 0.0f);

    ex::sim::ApplyBodyDrag(physics, {body}, {sample}, 0.1f);
    physics.Step(1.0f / 120.0f);
    CHECK(physics.Bodies()[size_t(index)].velocity.x > 0.1f);

    // Static bodies never receive an impulse.
    ex::scene::RigidBody wall;
    wall.position = DirectX::XMFLOAT3(3.0f, 5.0f, 0.0f);
    wall.halfExtents = rigid.halfExtents;
    wall.isStatic = true;
    const int wallIndex = physics.AddBody(wall);
    body.physicsIndex = wallIndex;
    ex::sim::ApplyBodyDrag(physics, {body}, {sample}, 0.1f);
    physics.Step(1.0f / 120.0f);
    CHECK(physics.Bodies()[size_t(wallIndex)].velocity.x == 0.0f);
}

void TestVolumeBakeObstacles() {
    ex::core::ThreadPool pool(2);
    ex::sim::BlastModel blast;
    blast.Configure({}, {});
    blast.Reset();
    for (int step = 0; step < 120; ++step) {
        blast.Advance(1.0 / 60.0);
    }

    ex::scene::SceneSettings settings{};
    settings.seed = 7;
    settings.density = 0.25f;
    settings.quality = 0;
    ex::scene::Scene scene;
    scene.Generate(settings, blast);

    ex::sim::VolumeBaker baker;
    baker.Initialize(&pool, 16);
    baker.SetScene(&scene);
    const ex::sim::VolumeBakeRequest request = baker.Prepare(blast);
    CHECK(request.hasSolidMask());
    CHECK(request.solidMaskResolution >= 8);
    CHECK(request.solidMaskResolution <= 32);

    baker.BakeNow(blast);
    CHECK(baker.HasData());
    const ex::sim::VolumeFrame& frame = baker.Current();
    CHECK(frame.resolution == request.resolution);
    CHECK(frame.rgba.size() ==
          size_t(frame.resolution) * size_t(frame.resolution) *
              size_t(frame.resolution) * 8u);

    // Every voxel the mask marks as solid must be empty in the baked frame.
    const int resolution = frame.resolution;
    const int maskResolution = request.solidMaskResolution;
    const ex::sim::VolumeBounds& maskBounds = request.solidMaskBounds;
    const DirectX::XMFLOAT3 bakeSize{
        request.bounds.max.x - request.bounds.min.x,
        request.bounds.max.y - request.bounds.min.y,
        request.bounds.max.z - request.bounds.min.z};
    const float maskCellX =
        (maskBounds.max.x - maskBounds.min.x) / float(maskResolution);
    const float maskCellY =
        (maskBounds.max.y - maskBounds.min.y) / float(maskResolution);
    const float maskCellZ =
        (maskBounds.max.z - maskBounds.min.z) / float(maskResolution);
    size_t solidVoxels = 0;
    size_t nonEmptySolidVoxels = 0;
    for (int z = 0; z < resolution; ++z) {
        const float worldZ =
            request.bounds.min.z +
            (float(z) + 0.5f) * bakeSize.z / float(resolution);
        const int mz = std::clamp(
            int((worldZ - maskBounds.min.z) / maskCellZ), 0,
            maskResolution - 1);
        for (int y = 0; y < resolution; ++y) {
            const float worldY =
                request.bounds.min.y +
                (float(y) + 0.5f) * bakeSize.y / float(resolution);
            const int my = std::clamp(
                int((worldY - maskBounds.min.y) / maskCellY), 0,
                maskResolution - 1);
            for (int x = 0; x < resolution; ++x) {
                const float worldX =
                    request.bounds.min.x +
                    (float(x) + 0.5f) * bakeSize.x / float(resolution);
                const int mx = std::clamp(
                    int((worldX - maskBounds.min.x) / maskCellX), 0,
                    maskResolution - 1);
                const size_t maskIndex =
                    (size_t(mz) * size_t(maskResolution) + size_t(my)) *
                        size_t(maskResolution) +
                    size_t(mx);
                if ((*request.solidMask)[maskIndex] <= 0.5f) {
                    continue;
                }
                ++solidVoxels;
                const size_t offset =
                    ((size_t(z) * size_t(resolution) + size_t(y)) *
                         size_t(resolution) +
                     size_t(x)) *
                    8u;
                for (size_t byte = 0; byte < 8; ++byte) {
                    if (frame.rgba[offset + byte] != 0) {
                        ++nonEmptySolidVoxels;
                        break;
                    }
                }
            }
        }
    }
    CHECK(nonEmptySolidVoxels == 0);
    CHECK(solidVoxels > 0);
}

} // namespace

int main() {
    TestBlastModel();
    TestSceneDeterminism();
    TestVolumeBake();
    TestObstacleVoxelization();
    TestObstacleVelocityMask();
    TestPhysicsObstacles();
    TestBodyDrag();
    TestVolumeObstacleCache();
    TestVolumeBakeObstacles();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d core test(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All core tests passed.\n");
    return 0;
}
