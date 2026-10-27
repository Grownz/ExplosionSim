#include "scene/SceneTypes.h"

#include "core/Log.h"
#include "core/Random.h"
#include "core/ThreadPool.h"
#include "scene/VoronoiFracture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <queue>
#include <utility>

namespace ex::scene {

namespace {

constexpr DirectX::XMFLOAT4 kBuildingColors[4] = {
    {0.62f, 0.58f, 0.52f, 1.0f},
    {0.52f, 0.52f, 0.50f, 1.0f},
    {0.56f, 0.40f, 0.32f, 1.0f},
    {0.70f, 0.68f, 0.62f, 1.0f},
};

uint64_t HashObject(uint64_t hash, const SceneObject& object) {
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    mix(object.id);
    mix(uint64_t(object.type));
    mix(uint64_t(int(std::lround(object.position.x * 100.0f))));
    mix(uint64_t(int(std::lround(object.position.z * 100.0f))));
    mix(uint64_t(int(std::lround(object.yaw * 10.0f))));
    mix(uint64_t(int(std::lround(object.size.x * 100.0f))));
    mix(uint64_t(int(std::lround(object.size.y * 100.0f))));
    mix(uint64_t(int(std::lround(object.size.z * 100.0f))));
    mix(uint64_t(object.meshVariant));
    mix(uint64_t(std::llround(double(object.peakOverpressurePa) * 10.0)));
    mix(uint64_t(std::llround(double(object.impulsePaS) * 10.0)));
    mix(uint64_t(std::llround(double(object.thermalFluenceJm2) * 10.0)));
    // Chunk geometry is part of the scene hash so the renderer rebuilds its
    // vertex/index buffers when the chunk scale or budget changes.
    mix(uint64_t(object.chunks.size()));
    for (const SceneChunk& chunk : object.chunks) {
        mix(uint64_t(int(std::lround(chunk.center.x * 100.0f))));
        mix(uint64_t(int(std::lround(chunk.center.y * 100.0f))));
        mix(uint64_t(int(std::lround(chunk.center.z * 100.0f))));
        mix(uint64_t(int(std::lround(chunk.half.x * 100.0f))));
        mix(uint64_t(int(std::lround(chunk.half.y * 100.0f))));
        mix(uint64_t(int(std::lround(chunk.half.z * 100.0f))));
        mix(uint64_t(int(std::lround(chunk.orientation.x * 1000.0f))));
        mix(uint64_t(int(std::lround(chunk.orientation.y * 1000.0f))));
        mix(uint64_t(int(std::lround(chunk.orientation.z * 1000.0f))));
        mix(uint64_t(int(std::lround(chunk.orientation.w * 1000.0f))));
        mix(uint64_t(int(std::lround(chunk.volume * 1000.0f))));
        mix(uint64_t(chunk.hullPositions.size()));
        mix(uint64_t(chunk.neighbors.size()));
    }
    return hash;
}

struct DamageThresholds {
    float pressureLight;
    float pressureHeavy;
    float pressureDestroy;
    float impulseLight;
    float impulseHeavy;
    float impulseDestroy;
};

DamageThresholds ThresholdsFor(SceneObjectType type) {
    switch (type) {
    case SceneObjectType::Building:
        return {7000.0f, 20000.0f, 45000.0f, 150.0f, 500.0f, 1500.0f};
    case SceneObjectType::Vehicle:
        return {10000.0f, 25000.0f, 50000.0f, 200.0f, 600.0f, 1500.0f};
    case SceneObjectType::Tree:
        return {8000.0f, 18000.0f, 35000.0f, 150.0f, 400.0f, 900.0f};
    default:
        return {1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f};
    }
}

DamageState EvaluateDamage(float pressure, float impulse,
                           const DamageThresholds& thresholds) {
    if (pressure >= thresholds.pressureDestroy || impulse >= thresholds.impulseDestroy) {
        return DamageState::Destroyed;
    }
    if (pressure >= thresholds.pressureHeavy || impulse >= thresholds.impulseHeavy) {
        return DamageState::Heavy;
    }
    if (pressure >= thresholds.pressureLight || impulse >= thresholds.impulseLight) {
        return DamageState::Light;
    }
    return DamageState::Intact;
}

// Slab test returning the parametric interval [t0, t1] of the segment inside the
// box. The interval is clamped to [0, 1]; false means the segment misses.
bool SegmentInterval(const DirectX::XMFLOAT3& start, const DirectX::XMFLOAT3& end,
                     const DirectX::XMFLOAT3& lower, const DirectX::XMFLOAT3& upper,
                     float& t0, float& t1) {
    t0 = 0.0f;
    t1 = 1.0f;
    const float origin[3] = {start.x, start.y, start.z};
    const float direction[3] = {end.x - start.x, end.y - start.y, end.z - start.z};
    const float boxMin[3] = {lower.x, lower.y, lower.z};
    const float boxMax[3] = {upper.x, upper.y, upper.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-6f) {
            if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis]) {
                return false;
            }
            continue;
        }
        float enter = (boxMin[axis] - origin[axis]) / direction[axis];
        float leave = (boxMax[axis] - origin[axis]) / direction[axis];
        if (enter > leave) {
            std::swap(enter, leave);
        }
        t0 = std::max(t0, enter);
        t1 = std::min(t1, leave);
        if (t0 > t1) {
            return false;
        }
    }
    return t1 >= 0.0f && t0 <= 1.0f;
}

// Places the k-th copy of a block-sized object on a small sub-grid so that
// density > 1 spreads the extra copies over the block instead of stacking them.
void SlotOffset(int index, int slotsPerAxis, float slotSize, float block, float& outX,
                float& outZ) {
    if (slotsPerAxis <= 1) {
        outX = 0.0f;
        outZ = 0.0f;
        return;
    }
    const int col = index % slotsPerAxis;
    const int row = (index / slotsPerAxis) % slotsPerAxis;
    outX = (float(col) + 0.5f) * slotSize - block * 0.5f;
    outZ = (float(row) + 0.5f) * slotSize - block * 0.5f;
}

// Grid fracture: jittered split planes give non-uniform chunks without changing
// the topology or the OBB physics. This is the default and the fast path.
void BuildGridChunks(SceneObject& object, const DirectX::XMFLOAT3& size, int quality,
                     core::Rng& rng) {
    const float x = object.position.x;
    const float z = object.position.z;
    const int nx = quality >= 2 ? 3 : (quality >= 1 ? 3 : 2);
    const int ny = quality >= 2 ? 5 : (quality >= 1 ? 4 : 3);
    const int nz = nx;
    const float sx = size.x / float(nx);
    const float sy = size.y / float(ny);
    const float sz = size.z / float(nz);

    // The jitter is clamped so no chunk becomes thinner than a quarter cell.
    std::vector<float> splitX(nx + 1);
    std::vector<float> splitY(ny + 1);
    std::vector<float> splitZ(nz + 1);
    const float minX = 0.28f * sx;
    const float minY = 0.28f * sy;
    const float minZ = 0.28f * sz;
    splitX[0] = x - size.x * 0.5f;
    splitX[nx] = x + size.x * 0.5f;
    for (int i = 1; i < nx; ++i) {
        const float base = x - size.x * 0.5f + size.x * float(i) / float(nx);
        const float jitter = rng.Range(-0.28f, 0.28f) * sx;
        splitX[i] = std::clamp(base + jitter, splitX[i - 1] + minX,
                               splitX[nx] - minX * float(nx - i));
    }
    splitY[0] = 0.0f;
    splitY[ny] = size.y;
    for (int i = 1; i < ny; ++i) {
        const float base = size.y * float(i) / float(ny);
        const float jitter = rng.Range(-0.28f, 0.28f) * sy;
        splitY[i] = std::clamp(base + jitter, splitY[i - 1] + minY,
                               splitY[ny] - minY * float(ny - i));
    }
    splitZ[0] = z - size.z * 0.5f;
    splitZ[nz] = z + size.z * 0.5f;
    for (int i = 1; i < nz; ++i) {
        const float base = z - size.z * 0.5f + size.z * float(i) / float(nz);
        const float jitter = rng.Range(-0.28f, 0.28f) * sz;
        splitZ[i] = std::clamp(base + jitter, splitZ[i - 1] + minZ,
                               splitZ[nz] - minZ * float(nz - i));
    }

    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            for (int iz = 0; iz < nz; ++iz) {
                SceneChunk chunk;
                chunk.center = DirectX::XMFLOAT3(
                    (splitX[ix] + splitX[ix + 1]) * 0.5f,
                    (splitY[iy] + splitY[iy + 1]) * 0.5f,
                    (splitZ[iz] + splitZ[iz + 1]) * 0.5f);
                chunk.half = DirectX::XMFLOAT3(
                    (splitX[ix + 1] - splitX[ix]) * 0.5f,
                    (splitY[iy + 1] - splitY[iy]) * 0.5f,
                    (splitZ[iz + 1] - splitZ[iz]) * 0.5f);
                chunk.volume = 8.0f * chunk.half.x * chunk.half.y * chunk.half.z;
                chunk.surfaceArea =
                    4.0f * (chunk.half.x * chunk.half.y +
                            chunk.half.y * chunk.half.z +
                            chunk.half.x * chunk.half.z);
                chunk.color = object.color;
                chunk.gridY = iy;
                object.chunks.push_back(chunk);
            }
        }
    }

    // Structural support graph: connect each chunk to its six grid neighbours.
    // The collapse pass breaks these joints and flood-fills the remaining
    // support from the ground.
    auto indexAt = [nx, ny, nz](int ix, int iy, int iz) {
        return (ix * ny + iy) * nz + iz;
    };
    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            for (int iz = 0; iz < nz; ++iz) {
                const int index = indexAt(ix, iy, iz);
                if (ix > 0) {
                    object.chunks[index].neighbors.push_back(
                        indexAt(ix - 1, iy, iz));
                }
                if (ix + 1 < nx) {
                    object.chunks[index].neighbors.push_back(
                        indexAt(ix + 1, iy, iz));
                }
                if (iy > 0) {
                    object.chunks[index].neighbors.push_back(
                        indexAt(ix, iy - 1, iz));
                }
                if (iy + 1 < ny) {
                    object.chunks[index].neighbors.push_back(
                        indexAt(ix, iy + 1, iz));
                }
                if (iz > 0) {
                    object.chunks[index].neighbors.push_back(
                        indexAt(ix, iy, iz - 1));
                }
                if (iz + 1 < nz) {
                    object.chunks[index].neighbors.push_back(
                        indexAt(ix, iy, iz + 1));
                }
            }
        }
    }
    for (SceneChunk& chunk : object.chunks) {
        chunk.neighborBroken.assign(chunk.neighbors.size(), 0);
    }
}

// Target number of Voronoi cells for a building. Kept in one place so the
// global chunk budget can scale the counts before the parallel fracture.
int VoronoiCellCount(const DirectX::XMFLOAT3& size, int quality, float chunkScale) {
    const float baseCellVolume =
        quality >= 2 ? 24.0f : (quality >= 1 ? 45.0f : 90.0f);
    const float scale = std::clamp(chunkScale, 0.25f, 2.0f);
    const float targetCellVolume = baseCellVolume * scale * scale * scale;
    const float volume = size.x * size.y * size.z;
    return std::clamp(int(volume / targetCellVolume + 0.5f), 6, 256);
}

// Voronoi fracture (roadmap 2.3 phase B): convex cells clipped to the building
// box. The cells keep a flat-shaded hull for rendering and a fitted OBB as the
// collision proxy, so the rigid-body solver is unchanged.
void BuildVoronoiChunks(SceneObject& object, const DirectX::XMFLOAT3& size, int quality,
                        int cellCount, uint32_t fractureSeed, core::Rng& rng) {
    std::vector<FractureCell> cells;
    const uint32_t cellSeed = fractureSeed ^ (object.id * 0x9E3779B9u);
    FractureBoxVoronoi(
        DirectX::XMFLOAT3(object.position.x, size.y * 0.5f, object.position.z),
        DirectX::XMFLOAT3(size.x * 0.5f, size.y * 0.5f, size.z * 0.5f), cellCount,
        cellSeed, cells);
    if (cells.empty()) {
        BuildGridChunks(object, size, quality, rng);
        return;
    }

    object.chunks.reserve(cells.size());
    for (FractureCell& cell : cells) {
        SceneChunk chunk;
        chunk.center = cell.center;
        chunk.half = cell.half;
        chunk.orientation = cell.orientation;
        chunk.volume = cell.volume;
        chunk.surfaceArea = cell.surfaceArea;
        chunk.gridY = cell.grounded ? 0 : 1;
        chunk.color = object.color;
        chunk.hullPositions = std::move(cell.positions);
        chunk.hullNormals = std::move(cell.normals);
        chunk.hullIndices = std::move(cell.indices);
        chunk.neighbors = std::move(cell.neighbors);
        object.chunks.push_back(std::move(chunk));
    }
    for (SceneChunk& chunk : object.chunks) {
        chunk.neighborBroken.assign(chunk.neighbors.size(), 0);
    }
}

} // namespace

void Scene::Clear() {
    settings_ = SceneSettings{};
    objects_.clear();
    debrisSpawns_.clear();
    fireSpawns_.clear();
    pendingFailures_.clear();
    bodyChunk_.clear();
    physics_.Clear();
    hash_ = 0;
    extent_ = 0.0f;
    time_ = 0.0;
    physicsAccumulator_ = 0.0;
}

void Scene::ResetState() {
    for (SceneObject& object : objects_) {
        object.damage = DamageState::Intact;
        object.damageAmount = 0.0f;
        object.resolved = false;
        object.burning = false;
        object.burnTimer = 0.0f;
        object.bodyIndex = -1;
        object.collapsed = false;
        object.collapsing = false;
        object.structureRegistered = false;
        for (SceneChunk& chunk : object.chunks) {
            chunk.bodyIndex = -1;
            chunk.detached = false;
            chunk.neighborBroken.assign(chunk.neighbors.size(), 0);
        }
    }
    debrisSpawns_.clear();
    fireSpawns_.clear();
    pendingFailures_.clear();
    bodyChunk_.clear();
    physics_.Clear();
    time_ = 0.0;
    physicsAccumulator_ = 0.0;
}

bool Scene::GenerateInternal(const SceneSettings& settings,
                             const sim::BlastModel& blast,
                             const std::function<bool(float)>& progress,
                             core::ThreadPool* pool, bool layoutOnly) {
    std::chrono::steady_clock::time_point generationStart{};
    if (!layoutOnly) {
        ++generationCount_;
        generationStart = std::chrono::steady_clock::now();
    }
    std::mutex progressMutex;
    float lastProgress = 0.0f;
    const auto report = [&](float value) {
        if (!progress) {
            return true;
        }
        std::lock_guard<std::mutex> lock(progressMutex);
        value = std::max(value, lastProgress);
        lastProgress = value;
        return progress(std::clamp(value, 0.0f, 1.0f));
    };
    if (!report(0.0f)) {
        return false;
    }

    settings_ = settings;
    objects_.clear();
    debrisSpawns_.clear();
    fireSpawns_.clear();
    pendingFailures_.clear();
    bodyChunk_.clear();
    physics_.Clear();
    hash_ = 0;
    extent_ = 0.0f;
    time_ = 0.0;
    physicsAccumulator_ = 0.0;

    if (!settings_.enabled) {
        report(1.0f);
        return true;
    }

    const sim::Charge& charge = blast.GetCharge();
    const double fireball = std::max(blast.FireballMaxRadiusM(), 0.1);
    const double minRadius = std::max({fireball * 2.0, charge.RadiusM() * 2.0, 6.0});

    const float block = 16.0f;
    const float road = 6.0f;
    const float pitch = block + road;

    // The town must extend beyond the destruction radius so that intact objects
    // remain visible outside the blast. The old layout was a square of half
    // extent `squareExtent`; the town is now the circumscribed disc whose radius
    // is that square's half diagonal.
    const double baseExtent = std::clamp(10.0 * fireball + 20.0, 45.0, 160.0);
    const double radiusExtent = minRadius * 1.4 + 2.0 * double(pitch);
    const double squareExtent = std::max(baseExtent, radiusExtent);
    constexpr double kCircleAreaRatio = 1.5707963267948966; // pi / 2
    const double townRadius = squareExtent * 1.4142135623730951; // sqrt(2)
    extent_ = float(townRadius);

    // Keep roughly the same object density as the old 160 m square town, but cap
    // the budget so a 100 kt scene still generates in a fraction of a second.
    const double density = double(std::clamp(settings_.density, 0.0f, 4.0f));
    const double extentScale = std::max(1.0, squareExtent / 160.0);
    const int maxBudget = 3200;
    const int budget = std::clamp(
        int(800.0 * density * extentScale * extentScale * kCircleAreaRatio), 0,
        maxBudget);
    if (budget == 0) {
        report(1.0f);
        return true;
    }

    const int buildingBudget = settings_.buildings ? int(budget * 0.45f) : 0;
    const int vehicleBudget = settings_.vehicles ? int(budget * 0.20f) : 0;
    const int treeBudget = settings_.vegetation
                               ? std::max(0, budget - buildingBudget - vehicleBudget)
                               : 0;

    core::Rng rng(uint64_t(settings_.seed) * 0x9E3779B97F4A7C15ull + 0xC0FFEEull);

    const int cells = int(extent_ / pitch) + 1;
    const double burstHeight = charge.heightOfBurstM;

    // Density > 1 packs several objects into every block; their footprint is
    // shrunk accordingly so they still fit without overlapping heavily.
    const int perBlock =
        std::max(1, int(std::ceil(std::clamp(settings_.density, 0.0f, 4.0f))));
    const int slotsPerAxis = int(std::ceil(std::sqrt(float(perBlock))));
    const float slotSize = block / float(slotsPerAxis);
    const float shrink = 1.0f / std::sqrt(float(perBlock));

    // For large scenes the budget is smaller than the block grid. Sample blocks
    // uniformly instead of filling one corner row by row until the budget runs
    // out, so the town surrounds the burst instead of clustering in a corner.
    // The per-type base rates (empty lots, parked cars, tree spacing) keep the
    // sampler from undershooting the category budget.
    int blockCount = 0;
    for (int ix = -cells; ix <= cells; ++ix) {
        for (int iz = -cells; iz <= cells; ++iz) {
            const float cx = ix * pitch + block * 0.5f;
            const float cz = iz * pitch + block * 0.5f;
            if (cx * cx + cz * cz <= extent_ * extent_) {
                ++blockCount;
            }
        }
    }
    const int blockSlots = std::max(1, blockCount * perBlock);
    const auto keepChance = [&](int categoryBudget, float baseKeep) {
        if (categoryBudget <= 0) {
            return 0.0f;
        }
        const float target = 0.9f * float(categoryBudget) /
                             (float(blockSlots) * baseKeep);
        return std::min(1.0f, target);
    };
    const float buildingChance = keepChance(buildingBudget, 0.85f);
    const float vehicleChance = keepChance(vehicleBudget, 0.45f);
    const float treeChance = keepChance(treeBudget, 0.55f);
    if (!report(0.02f)) {
        return false;
    }

    auto addObject = [&](SceneObjectType type, float x, float z, float yaw,
                         DirectX::XMFLOAT3 size, int variant) {
        SceneObject object;
        object.type = type;
        object.position = DirectX::XMFLOAT3(x, 0.0f, z);
        object.yaw = yaw;
        object.size = size;
        object.meshVariant = variant;
        object.swayPhase = rng.Float01() * 6.2831853f;
        object.color = type == SceneObjectType::Building
                           ? kBuildingColors[variant & 3]
                           : DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

        // Keep the town inside the circular footprint; the block grid is a
        // bounding square, so edge objects are rejected here.
        const double radialSq = double(x) * double(x) + double(z) * double(z);
        if (radialSq > double(extent_) * double(extent_)) {
            return;
        }

        // Everything inside the destruction radius is gone before the first
        // frame. Reject it before fracturing the building, which keeps huge
        // scenes cheap and avoids generating chunks that are thrown away.
        const double dx = double(x);
        const double dz = double(z);
        const double dy = double(size.y) * 0.5 - burstHeight;
        object.distanceToBurst = float(std::sqrt(dx * dx + dz * dz + dy * dy));
        if (object.distanceToBurst < minRadius) {
            return;
        }

        object.id = uint32_t(objects_.size());
        if (type == SceneObjectType::Building) {
            const int quality = std::clamp(settings_.quality, 0, 2);
            if (settings_.voronoiChunks) {
                // Draw the fracture seed in the same serial order as before;
                // the actual fracture runs later, in parallel.
                object.fractureSeed = rng.NextU32();
                object.requestedChunks =
                    VoronoiCellCount(size, quality, settings_.chunkScale);
                object.chunkCount = object.requestedChunks;
            } else {
                BuildGridChunks(object, size, quality, rng);
            }
        }

        object.arrivalTime = float(blast.EstimateArrivalTimeS(object.distanceToBurst));
        object.peakOverpressurePa =
            float(blast.PeakOverpressureAtDistance(object.distanceToBurst));
        object.impulsePaS = float(blast.OverpressureImpulsePaS(object.distanceToBurst));
        object.thermalFluenceJm2 =
            float(blast.ThermalFluenceJm2(object.distanceToBurst));
        objects_.push_back(object);
    };

    int buildings = 0;
    for (int ix = -cells; ix <= cells && buildings < buildingBudget; ++ix) {
        for (int iz = -cells; iz <= cells && buildings < buildingBudget; ++iz) {
            const float cx = ix * pitch + block * 0.5f;
            const float cz = iz * pitch + block * 0.5f;
            if (cx * cx + cz * cz > extent_ * extent_) {
                continue;
            }
            if (buildingChance < 1.0f && rng.Float01() >= buildingChance) {
                continue;
            }
            if (rng.Float01() < 0.15f) {
                continue; // occasional empty lot
            }

            // Density > 1 packs several smaller buildings into one block.
            for (int slot = 0; slot < perBlock && buildings < buildingBudget; ++slot) {
                float slotX = 0.0f;
                float slotZ = 0.0f;
                SlotOffset(slot, slotsPerAxis, slotSize, block, slotX, slotZ);
                const float jitterX = rng.Range(-3.0f, 3.0f) * shrink;
                const float jitterZ = rng.Range(-3.0f, 3.0f) * shrink;
                const float width = rng.Range(6.0f, 12.0f) * shrink;
                const float depth = rng.Range(6.0f, 12.0f) * shrink;
                const float height = rng.Range(6.0f, 26.0f);
                const float yaw = 0.0f;
                addObject(SceneObjectType::Building, cx + slotX + jitterX,
                          cz + slotZ + jitterZ, yaw,
                          DirectX::XMFLOAT3(width, height, depth),
                          int(rng.NextU32() % 4));
                ++buildings;
            }
        }
        if (!report(0.02f + 0.04f * float(ix + cells + 1) /
                                 float(2 * cells + 1))) {
            return false;
        }
    }
    if (!report(0.06f)) {
        return false;
    }

    int vehicles = 0;
    for (int ix = -cells; ix <= cells && vehicles < vehicleBudget; ++ix) {
        for (int iz = -cells; iz <= cells && vehicles < vehicleBudget; ++iz) {
            if (vehicleChance < 1.0f && rng.Float01() >= vehicleChance) {
                continue;
            }
            if (rng.Float01() < 0.55f) {
                continue;
            }
            const bool alongX = rng.Float01() < 0.5f;
            const float x = alongX ? ix * pitch + block + road * 0.5f
                                   : ix * pitch + rng.Range(0.0f, block);
            const float z = alongX ? iz * pitch + rng.Range(0.0f, block)
                                   : iz * pitch + block + road * 0.5f;
            if (x * x + z * z > extent_ * extent_) {
                continue;
            }
            const float yaw = alongX ? 90.0f : 0.0f;
            for (int slot = 0; slot < perBlock && vehicles < vehicleBudget; ++slot) {
                const float spread = slot * 1.4f;
                addObject(SceneObjectType::Vehicle,
                          x + (alongX ? 0.0f : spread) - (alongX ? spread : 0.0f),
                          z + (alongX ? spread : 0.0f) - (alongX ? 0.0f : spread),
                          yaw,
                          DirectX::XMFLOAT3(rng.Range(1.7f, 2.2f), rng.Range(1.3f, 2.4f),
                                            rng.Range(3.8f, 6.5f)),
                          int(rng.NextU32() % 3));
                ++vehicles;
            }
        }
        if (!report(0.06f + 0.02f * float(ix + cells + 1) /
                                 float(2 * cells + 1))) {
            return false;
        }
    }
    if (!report(0.08f)) {
        return false;
    }

    int trees = 0;
    for (int ix = -cells; ix <= cells && trees < treeBudget; ++ix) {
        for (int iz = -cells; iz <= cells && trees < treeBudget; ++iz) {
            if (treeChance < 1.0f && rng.Float01() >= treeChance) {
                continue;
            }
            if (rng.Float01() < 0.45f) {
                continue;
            }
            const float x = ix * pitch + block + road * 0.5f +
                            rng.Range(-road * 0.4f, road * 0.4f);
            const float z = iz * pitch + block + road * 0.5f +
                            rng.Range(-road * 0.4f, road * 0.4f);
            if (x * x + z * z > extent_ * extent_) {
                continue;
            }
            for (int slot = 0; slot < perBlock && trees < treeBudget; ++slot) {
                float slotX = 0.0f;
                float slotZ = 0.0f;
                SlotOffset(slot, slotsPerAxis, slotSize, block, slotX, slotZ);
                const float height = rng.Range(5.0f, 12.0f);
                addObject(SceneObjectType::Tree, x + slotX * 0.5f, z + slotZ * 0.5f,
                          rng.Range(0.0f, 360.0f),
                          DirectX::XMFLOAT3(rng.Range(2.0f, 4.5f), height,
                                            rng.Range(0.25f, 0.45f)),
                          int(rng.NextU32() % 3));
                ++trees;
            }
        }
        if (!report(0.08f + 0.02f * float(ix + cells + 1) /
                                 float(2 * cells + 1))) {
            return false;
        }
    }
    if (!report(0.10f)) {
        return false;
    }
    if (layoutOnly) {
        // A layout hash lets the renderer invalidate the static shadow map and
        // chunk buffers between two different previews.
        hash_ = 1469598103934665603ull;
        for (const SceneObject& object : objects_) {
            hash_ = HashObject(hash_, object);
        }
        return true;
    }

    // Global chunk budget. The requested Voronoi count grows quickly at small
    // chunk scales; the O(N^2) clipping would otherwise dominate the loading
    // time. Scale the per-building counts down before fracturing.
    int buildingCount = 0;
    int requestedChunks = 0;
    if (settings_.voronoiChunks) {
        for (const SceneObject& object : objects_) {
            if (object.type == SceneObjectType::Building) {
                ++buildingCount;
                requestedChunks += object.requestedChunks;
            }
        }
        if (requestedChunks > kSceneChunkBudget && buildingCount > 0) {
            const float scale = float(kSceneChunkBudget) / float(requestedChunks);
            int total = 0;
            for (SceneObject& object : objects_) {
                if (object.type != SceneObjectType::Building) {
                    continue;
                }
                object.chunkCount =
                    std::max(6, int(float(object.requestedChunks) * scale));
                total += object.chunkCount;
            }
            // Trim the rounding remainder (and the min-6 floor) from buildings
            // that still have room.
            for (int pass = 0; pass < 4 && total > kSceneChunkBudget; ++pass) {
                for (SceneObject& object : objects_) {
                    if (total <= kSceneChunkBudget) {
                        break;
                    }
                    if (object.type == SceneObjectType::Building &&
                        object.chunkCount > 6) {
                        --object.chunkCount;
                        --total;
                    }
                }
            }
            LogInfo("Scene chunk budget: requested %d, capped to %d", requestedChunks,
                    total);
        }
    }

    // Fracture the buildings in parallel. Each building uses its own seed and
    // RNG, so the result is independent of the thread schedule.
    const int quality = std::clamp(settings_.quality, 0, 2);
    std::atomic<bool> cancelled{false};
    std::atomic<int> completed{0};
    const auto fractureOne = [&](SceneObject& object) {
        core::Rng local(object.fractureSeed);
        BuildVoronoiChunks(object, object.size, quality, object.chunkCount,
                           object.fractureSeed, local);
    };
    if (settings_.voronoiChunks && buildingCount > 0) {
        if (pool) {
            pool->ParallelFor(int(objects_.size()), 1, [&](int begin, int end) {
                for (int i = begin; i < end; ++i) {
                    if (cancelled.load(std::memory_order_relaxed)) {
                        return;
                    }
                    SceneObject& object = objects_[i];
                    if (object.type != SceneObjectType::Building) {
                        continue;
                    }
                    fractureOne(object);
                    const int done = ++completed;
                    if ((done & 7) == 0 || done == buildingCount) {
                        if (!report(0.10f + 0.70f * float(done) /
                                                float(buildingCount))) {
                            cancelled.store(true, std::memory_order_relaxed);
                            return;
                        }
                    }
                }
            });
        } else {
            for (SceneObject& object : objects_) {
                if (object.type != SceneObjectType::Building) {
                    continue;
                }
                fractureOne(object);
                const int done = ++completed;
                if ((done & 7) == 0 || done == buildingCount) {
                    if (!report(0.10f + 0.70f * float(done) /
                                            float(buildingCount))) {
                        cancelled.store(true);
                        return false;
                    }
                }
            }
        }
        if (cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
    }
    if (!report(0.80f)) {
        return false;
    }

    // Blast shielding: nine rays over the receiver's bounding box, each
    // attenuated by the optical depth of every building it crosses. A
    // diffraction floor keeps receivers in the shadow from going to zero.
    struct BuildingAabb {
        DirectX::XMFLOAT3 min;
        DirectX::XMFLOAT3 max;
        uint32_t id;
    };
    std::vector<BuildingAabb> buildingBoxes;
    for (const SceneObject& object : objects_) {
        if (object.type != SceneObjectType::Building) {
            continue;
        }
        BuildingAabb box;
        box.id = object.id;
        box.min = DirectX::XMFLOAT3(object.position.x - object.size.x * 0.5f, 0.0f,
                                    object.position.z - object.size.z * 0.5f);
        box.max = DirectX::XMFLOAT3(object.position.x + object.size.x * 0.5f,
                                    object.size.y,
                                    object.position.z + object.size.z * 0.5f);
        buildingBoxes.push_back(box);
    }
    if (!report(0.82f)) {
        return false;
    }

    constexpr float kScreeningLength = 3.0f;   // structural screening length, m
    constexpr float kDiffractionLength = 12.0f; // ~c * positive phase, m
    const float groundReflection = std::clamp(settings_.groundReflection, 0.0f, 1.0f);
    const DirectX::XMFLOAT3 burstPoint(0.0f, float(burstHeight), 0.0f);
    const float objectCount = float(std::max<size_t>(1, objects_.size()));
    size_t shielded = 0;

    for (SceneObject& object : objects_) {
        const DirectX::XMFLOAT3 center(object.position.x, object.size.y * 0.5f,
                                       object.position.z);
        const DirectX::XMFLOAT3 half(object.size.x * 0.5f, object.size.y * 0.5f,
                                     object.size.z * 0.5f);

        float transmissionSum = 0.0f;
        float reflectionSum = 0.0f;
        int blockedRays = 0;
        for (int sx = -1; sx <= 1; ++sx) {
            for (int sy = -1; sy <= 1; ++sy) {
                for (int sz = -1; sz <= 1; sz += 2) {
                    const DirectX::XMFLOAT3 sample(
                        center.x + float(sx) * half.x,
                        std::max(0.2f, center.y + float(sy) * half.y),
                        center.z + float(sz) * half.z);

                    const DirectX::XMFLOAT3 delta(sample.x - burstPoint.x,
                                                  sample.y - burstPoint.y,
                                                  sample.z - burstPoint.z);
                    const float length =
                        std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                  delta.z * delta.z);

                    float opticalDepth = 0.0f;
                    for (const BuildingAabb& box : buildingBoxes) {
                        if (box.id == object.id) {
                            continue;
                        }
                        float t0 = 0.0f;
                        float t1 = 1.0f;
                        if (SegmentInterval(burstPoint, sample, box.min, box.max, t0,
                                            t1)) {
                            opticalDepth += std::max(0.0f, t1 - t0) * length;
                        }
                    }

                    float transmission =
                        std::clamp(std::exp(-opticalDepth / kScreeningLength), 0.04f,
                                   1.0f);
                    // Knife-edge diffraction around the nearest occluder.
                    const float diffraction =
                        0.5f * std::exp(-opticalDepth / kDiffractionLength);
                    transmission = std::max(transmission, diffraction);
                    if (opticalDepth > 0.05f) {
                        ++blockedRays;
                    }
                    transmissionSum += transmission;
                    // Surface reflection only matters close to the ground.
                    reflectionSum +=
                        1.0f + groundReflection * std::exp(-sample.y / 8.0f);
                }
            }
        }

        constexpr float kSampleCount = 18.0f; // 3 x 3 x 2
        const float factor = transmissionSum / kSampleCount;
        const float reflection = reflectionSum / kSampleCount;
        object.occluders = blockedRays;
        object.peakOverpressurePa *= factor * reflection;
        object.impulsePaS *= factor * reflection;
        object.thermalFluenceJm2 *= factor;

        if ((++shielded & 63u) == 0u &&
            !report(0.82f + 0.17f * float(shielded) / objectCount)) {
            return false;
        }
    }
    if (!report(0.99f)) {
        return false;
    }

    hash_ = 1469598103934665603ull;
    for (const SceneObject& object : objects_) {
        hash_ = HashObject(hash_, object);
    }
    report(1.0f);
    int chunkCount = 0;
    for (const SceneObject& object : objects_) {
        chunkCount += int(object.chunks.size());
    }
    LogInfo("Scene generated in %.1f ms: %d objects, %d chunks",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - generationStart)
                .count(),
            int(objects_.size()), chunkCount);
    return true;
}

void Scene::Generate(const SceneSettings& settings, const sim::BlastModel& blast,
                     const std::function<bool(float)>& progress,
                     core::ThreadPool* pool) {
    GenerateInternal(settings, blast, progress, pool, false);
}

bool Scene::GenerateLayout(const SceneSettings& settings,
                           const sim::BlastModel& blast,
                           const std::function<bool(float)>& progress) {
    return GenerateInternal(settings, blast, progress, nullptr, true);
}

void Scene::Update(const sim::BlastModel&, float dtSim) {
    if (dtSim > 0.0f) {
        time_ += double(dtSim);
    }
    ProcessJointFailures();

    for (SceneObject& object : objects_) {
        if (object.resolved) {
            continue;
        }
        if (time_ + 1e-4 < double(object.arrivalTime)) {
            continue;
        }

        object.resolved = true;
        const DamageThresholds thresholds = ThresholdsFor(object.type);
        object.damage = EvaluateDamage(object.peakOverpressurePa, object.impulsePaS,
                                       thresholds);

        switch (object.damage) {
        case DamageState::Light:
            object.damageAmount = 0.30f;
            break;
        case DamageState::Heavy:
            object.damageAmount = 0.65f;
            break;
        case DamageState::Destroyed:
            object.damageAmount = 1.00f;
            break;
        default:
            object.damageAmount = 0.0f;
            break;
        }

        if (object.damage == DamageState::Heavy ||
            object.damage == DamageState::Destroyed) {
            const bool destroyed = object.damage == DamageState::Destroyed;
            SceneDebrisSpawn spawn;
            spawn.position = DirectX::XMFLOAT3(object.position.x, object.size.y * 0.5f,
                                               object.position.z);
            spawn.count = object.type == SceneObjectType::Building
                              ? (destroyed ? 14 : 6)
                              : (destroyed ? 8 : 4);
            spawn.scale = (object.type == SceneObjectType::Building ? 0.55f : 0.30f) *
                          (destroyed ? 1.0f : 0.7f);
            spawn.speed = destroyed ? 22.0f : 12.0f;
            debrisSpawns_.push_back(spawn);
        }

        const float ignition = object.type == SceneObjectType::Tree
                                   ? 5000.0f
                                   : (object.type == SceneObjectType::Vehicle ? 15000.0f
                                                                             : 10000.0f);
        if (object.thermalFluenceJm2 >= ignition) {
            object.burning = true;
        }

        if (object.type == SceneObjectType::Building) {
            if (object.damage == DamageState::Heavy ||
                object.damage == DamageState::Destroyed) {
                StartCollapse(object);
            }
        } else if ((object.damage == DamageState::Heavy ||
                    object.damage == DamageState::Destroyed) &&
                   object.bodyIndex < 0) {
            ActivateBody(object);
        }
    }

    for (SceneObject& object : objects_) {
        if (!object.burning) {
            continue;
        }
        object.burnTimer += dtSim;
        if (object.burnTimer >= 0.25f) {
            object.burnTimer -= 0.25f;
            SceneFireSpawn spawn;
            // Spawn on the visible surface, not at the object centre: a flame
            // billboard buried inside the mesh is fully hidden by the soft
            // depth fade. Trees use the canopy rim, vehicles the roof and
            // buildings the roof rim.
            const float phase = float(object.id) * 1.7f + float(time_) * 2.3f;
            DirectX::XMFLOAT3 offset{};
            if (object.type == SceneObjectType::Tree) {
                offset = DirectX::XMFLOAT3(std::cos(phase) * object.size.x * 0.62f,
                                           object.size.y * 0.16f,
                                           std::sin(phase) * object.size.x * 0.62f);
                spawn.scale = 0.8f;
            } else if (object.type == SceneObjectType::Vehicle) {
                offset = DirectX::XMFLOAT3(std::cos(phase) * object.size.x * 0.30f,
                                           object.size.y * 0.28f,
                                           std::sin(phase) * object.size.z * 0.30f);
                spawn.scale = 0.5f;
            } else {
                offset = DirectX::XMFLOAT3(std::cos(phase) * object.size.x * 0.30f,
                                           object.size.y * 0.38f,
                                           std::sin(phase) * object.size.z * 0.30f);
                spawn.scale = 0.5f;
            }

            DirectX::XMFLOAT3 center(object.position.x, object.size.y * 0.5f,
                                     object.position.z);
            if (object.bodyIndex >= 0 &&
                object.bodyIndex < int(physics_.Bodies().size())) {
                // Burning trees and vehicles may have been thrown by the
                // blast; follow their current rigid-body pose.
                const RigidBody& body = physics_.Bodies()[object.bodyIndex];
                const DirectX::XMVECTOR rotated = DirectX::XMVector3Rotate(
                    DirectX::XMLoadFloat3(&offset),
                    DirectX::XMLoadFloat4(&body.orientation));
                DirectX::XMStoreFloat3(&offset, rotated);
                center = body.position;
            }
            spawn.position = DirectX::XMFLOAT3(center.x + offset.x,
                                               center.y + offset.y,
                                               center.z + offset.z);
            spawn.count = object.type == SceneObjectType::Tree ? 3 : 2;
            fireSpawns_.push_back(spawn);
        }
    }

    StepPhysics(dtSim);
}

void Scene::ActivateBody(SceneObject& object) {
    if (object.bodyIndex >= 0) {
        return;
    }

    RigidBody body;
    body.position = DirectX::XMFLOAT3(object.position.x, object.size.y * 0.5f,
                                      object.position.z);
    if (object.type == SceneObjectType::Tree) {
        body.halfExtents = DirectX::XMFLOAT3(object.size.x, object.size.y * 0.5f,
                                             object.size.x);
    } else {
        body.halfExtents = DirectX::XMFLOAT3(object.size.x * 0.5f,
                                             object.size.y * 0.5f,
                                             object.size.z * 0.5f);
    }
    body.boundingRadius = std::sqrt(body.halfExtents.x * body.halfExtents.x +
                                    body.halfExtents.y * body.halfExtents.y +
                                    body.halfExtents.z * body.halfExtents.z);
    body.mass = object.type == SceneObjectType::Vehicle ? 1500.0f : 600.0f;
    body.invMass = 1.0f / body.mass;

    const float halfYaw = object.yaw * 3.14159265f / 360.0f;
    body.orientation = DirectX::XMFLOAT4(0.0f, std::sin(halfYaw), 0.0f,
                                         std::cos(halfYaw));

    const float radialLength = std::sqrt(object.position.x * object.position.x +
                                         object.position.z * object.position.z);
    const float nx = radialLength > 1e-3f ? object.position.x / radialLength : 1.0f;
    const float nz = radialLength > 1e-3f ? object.position.z / radialLength : 0.0f;
    float speed = std::clamp(object.peakOverpressurePa / 5000.0f, 1.0f, 20.0f);
    if (object.damage == DamageState::Heavy) {
        speed *= 0.6f;
    }
    body.velocity = DirectX::XMFLOAT3(nx * speed, speed * 0.35f, nz * speed);
    const float sign = (object.id & 1u) ? 1.0f : -1.0f;
    body.angularVelocity = DirectX::XMFLOAT3(sign * 1.5f, sign * 0.7f, -sign * 1.1f);
    body.restitution = 0.25f;
    body.friction = 0.7f;

    object.bodyIndex = physics_.AddBody(body);
}

void Scene::StartCollapse(SceneObject& object) {
    if (object.collapsed || object.collapsing || object.chunks.empty()) {
        return;
    }
    object.collapsing = true;

    const int count = int(object.chunks.size());
    const float peak = object.peakOverpressurePa;
    const bool destroyed = object.damage == DamageState::Destroyed;
    // Joints are weaker once the building is in the destroyed state.
    const float jointStrength = destroyed ? 9000.0f : 26000.0f;

    const float radialLength = std::sqrt(object.position.x * object.position.x +
                                         object.position.z * object.position.z);
    const float burstX = -object.position.x / std::max(radialLength, 1e-3f);
    const float burstZ = -object.position.z / std::max(radialLength, 1e-3f);

    std::vector<float> load(count, 0.0f);
    for (int i = 0; i < count; ++i) {
        const SceneChunk& chunk = object.chunks[i];
        const float heightFraction =
            std::clamp(chunk.center.y / std::max(object.size.y, 0.1f), 0.0f, 1.0f);
        const float dx = chunk.center.x - object.position.x;
        const float dz = chunk.center.z - object.position.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        float facing = 0.5f;
        if (horizontal > 1e-3f) {
            facing = 0.5f + 0.5f * ((dx * burstX + dz * burstZ) / horizontal);
        }
        load[i] = peak * (0.65f + 0.35f * heightFraction) * (0.70f + 0.60f * facing);
    }

    // Stagger the joint failures: strongly loaded joints break almost at once,
    // lightly loaded ones follow over the next few tenths of a second.
    const int objectIndex = int(&object - objects_.data());
    for (int i = 0; i < count; ++i) {
        const SceneChunk& chunk = object.chunks[i];
        for (size_t n = 0; n < chunk.neighbors.size(); ++n) {
            const int j = chunk.neighbors[n];
            if (j < i) {
                continue;
            }
            const float stress = 0.5f * (load[i] + load[j]);
            const float ratio = stress / jointStrength;
            if (ratio >= 1.0f) {
                // Overstressed joints fail immediately; the flood fill below
                // detaches whatever they were carrying.
                BreakJoint(object, i, j);
                continue;
            }
            if (ratio < 0.15f) {
                continue;
            }
            PendingJointFailure failure;
            failure.time = time_ + 0.05 + 0.45 * (1.0 - std::min(ratio, 1.0f));
            failure.objectIndex = objectIndex;
            failure.chunkA = i;
            failure.chunkB = j;
            pendingFailures_.push_back(failure);
        }
    }

    const std::vector<uint8_t> supported = ComputeSupport(object);
    RegisterStructureBodies(object, supported);
    for (int i = 0; i < count; ++i) {
        if (!supported[i] && !object.chunks[i].detached) {
            DetachChunk(object, i);
        }
    }
}

void Scene::BreakJoint(SceneObject& object, int a, int b) {
    auto mark = [&](int from, int to) {
        SceneChunk& chunk = object.chunks[from];
        for (size_t i = 0; i < chunk.neighbors.size(); ++i) {
            if (chunk.neighbors[i] == to) {
                chunk.neighborBroken[i] = 1;
                return;
            }
        }
    };
    if (a < 0 || a >= int(object.chunks.size()) || b < 0 ||
        b >= int(object.chunks.size())) {
        return;
    }
    mark(a, b);
    mark(b, a);
}

void Scene::UpdateSupport(SceneObject& object) {
    const std::vector<uint8_t> supported = ComputeSupport(object);
    for (int i = 0; i < int(object.chunks.size()); ++i) {
        if (!supported[i] && !object.chunks[i].detached) {
            DetachChunk(object, i);
        }
    }
}

std::vector<uint8_t> Scene::ComputeSupport(const SceneObject& object) const {
    const int count = int(object.chunks.size());
    std::vector<uint8_t> supported(count, 0);
    std::queue<int> queue;
    for (int i = 0; i < count; ++i) {
        if (object.chunks[i].gridY == 0 && !object.chunks[i].detached) {
            supported[i] = 1;
            queue.push(i);
        }
    }
    while (!queue.empty()) {
        const int current = queue.front();
        queue.pop();
        const SceneChunk& chunk = object.chunks[current];
        if (chunk.neighborBroken.size() != chunk.neighbors.size()) {
            continue;
        }
        for (size_t n = 0; n < chunk.neighbors.size(); ++n) {
            if (n < chunk.neighborBroken.size() && chunk.neighborBroken[n]) {
                continue;
            }
            const int next = chunk.neighbors[n];
            if (next < 0 || next >= count) {
                continue;
            }
            if (supported[next] || object.chunks[next].detached) {
                continue;
            }
            supported[next] = 1;
            queue.push(next);
        }
    }
    return supported;
}

void Scene::DetachChunk(SceneObject& object, int index) {
    SceneChunk& chunk = object.chunks[index];
    if (chunk.detached) {
        return;
    }
    chunk.detached = true;
    object.collapsed = true;

    const float peak = object.peakOverpressurePa;
    const bool destroyed = object.damage == DamageState::Destroyed;
    const float radialLength = std::sqrt(object.position.x * object.position.x +
                                         object.position.z * object.position.z);
    const float burstX = -object.position.x / std::max(radialLength, 1e-3f);
    const float burstZ = -object.position.z / std::max(radialLength, 1e-3f);
    const float baseSpeed =
        std::clamp(peak / 8000.0f, 1.0f, 12.0f) * (destroyed ? 1.0f : 0.6f);
    const float volume = chunk.volume > 0.0f
                             ? chunk.volume
                             : 8.0f * chunk.half.x * chunk.half.y * chunk.half.z;
    const float mass = std::clamp(volume * 350.0f, 80.0f, 15000.0f);
    const DirectX::XMFLOAT3 velocity(burstX * baseSpeed * 0.8f, baseSpeed * 0.25f,
                                     burstZ * baseSpeed * 0.8f);
    const DirectX::XMFLOAT3 angular(chunk.center.x * 0.08f, chunk.center.z * 0.08f,
                                    chunk.center.y * 0.05f);

    if (chunk.bodyIndex >= 0) {
        physics_.SetBodyDynamic(chunk.bodyIndex, mass, velocity, angular);
        return;
    }

    RigidBody body;
    body.position = chunk.center;
    body.halfExtents = chunk.half;
    body.boundingRadius = std::sqrt(chunk.half.x * chunk.half.x +
                                    chunk.half.y * chunk.half.y +
                                    chunk.half.z * chunk.half.z);
    body.mass = mass;
    body.orientation = chunk.orientation;
    body.velocity = velocity;
    body.angularVelocity = angular;
    body.restitution = 0.12f;
    body.friction = 0.8f;
    chunk.bodyIndex = physics_.AddBody(body);

    const int objectIndex = int(&object - objects_.data());
    if (int(bodyChunk_.size()) <= chunk.bodyIndex) {
        bodyChunk_.resize(chunk.bodyIndex + 1, std::pair<int, int>(-1, -1));
    }
    bodyChunk_[chunk.bodyIndex] = {objectIndex, index};
}

void Scene::RegisterStructureBodies(SceneObject& object,
                                    const std::vector<uint8_t>& supported) {
    if (object.structureRegistered) {
        return;
    }
    object.structureRegistered = true;
    const int objectIndex = int(&object - objects_.data());
    for (int i = 0; i < int(object.chunks.size()); ++i) {
        SceneChunk& chunk = object.chunks[i];
        if (chunk.bodyIndex >= 0 || !supported[i]) {
            continue;
        }
        RigidBody body;
        body.position = chunk.center;
        body.halfExtents = chunk.half;
        body.orientation = chunk.orientation;
        body.boundingRadius = std::sqrt(chunk.half.x * chunk.half.x +
                                        chunk.half.y * chunk.half.y +
                                        chunk.half.z * chunk.half.z);
        body.mass = 1.0f;
        body.isStatic = true;
        body.friction = 0.8f;
        body.restitution = 0.1f;
        chunk.bodyIndex = physics_.AddBody(body);
        if (int(bodyChunk_.size()) <= chunk.bodyIndex) {
            bodyChunk_.resize(chunk.bodyIndex + 1, std::pair<int, int>(-1, -1));
        }
        bodyChunk_[chunk.bodyIndex] = {objectIndex, i};
    }
}

void Scene::ProcessJointFailures() {
    for (size_t i = 0; i < pendingFailures_.size();) {
        if (pendingFailures_[i].time > time_) {
            ++i;
            continue;
        }
        const PendingJointFailure failure = pendingFailures_[i];
        pendingFailures_[i] = pendingFailures_.back();
        pendingFailures_.pop_back();
        if (failure.objectIndex < 0 ||
            failure.objectIndex >= int(objects_.size())) {
            continue;
        }
        SceneObject& object = objects_[failure.objectIndex];
        BreakJoint(object, failure.chunkA, failure.chunkB);
        UpdateSupport(object);
    }
}

void Scene::ApplyContactDamage(float dtSim) {
    if (dtSim <= 0.0f) {
        return;
    }
    const auto& bodies = physics_.Bodies();
    for (const ContactEvent& event : physics_.ContactEvents()) {
        int staticBody = -1;
        if (event.bodyA >= 0 && event.bodyA < int(bodies.size()) &&
            bodies[event.bodyA].isStatic) {
            staticBody = event.bodyA;
        } else if (event.bodyB >= 0 && event.bodyB < int(bodies.size()) &&
                   bodies[event.bodyB].isStatic) {
            staticBody = event.bodyB;
        }
        if (staticBody < 0 || staticBody >= int(bodyChunk_.size())) {
            continue;
        }
        const auto location = bodyChunk_[staticBody];
        if (location.first < 0 || location.first >= int(objects_.size())) {
            continue;
        }
        SceneObject& object = objects_[location.first];
        if (location.second < 0 || location.second >= int(object.chunks.size())) {
            continue;
        }
        const SceneChunk& chunk = object.chunks[location.second];
        if (chunk.detached) {
            continue;
        }
        const float area =
            chunk.surfaceArea > 0.0f
                ? chunk.surfaceArea
                : 4.0f * (chunk.half.x * chunk.half.y +
                          chunk.half.y * chunk.half.z +
                          chunk.half.x * chunk.half.z);
        const float pressure =
            event.impulse / (std::max(area, 0.1f) * dtSim);
        const float strength =
            object.damage == DamageState::Destroyed ? 9000.0f : 26000.0f;
        if (pressure < strength * 0.35f) {
            continue;
        }
        // A heavy impact breaks the joints of the struck chunk after a short
        // delay, which can start a local collapse cascade.
        const int objectIndex = location.first;
        for (size_t n = 0; n < chunk.neighbors.size(); ++n) {
            if (chunk.neighborBroken[n]) {
                continue;
            }
            PendingJointFailure failure;
            failure.time = time_ + 0.03;
            failure.objectIndex = objectIndex;
            failure.chunkA = location.second;
            failure.chunkB = chunk.neighbors[n];
            pendingFailures_.push_back(failure);
        }
    }
}

void Scene::StepPhysics(float dtSim) {
    if (dtSim <= 0.0f) {
        return;
    }
    physicsAccumulator_ += double(dtSim);
    const double step = 1.0 / 120.0;
    int guard = 0;
    while (physicsAccumulator_ >= step && guard++ < 8) {
        physics_.Step(float(step));
        ApplyContactDamage(float(step));
        physicsAccumulator_ -= step;
    }
}

std::vector<SceneDebrisSpawn> Scene::ConsumeDebrisSpawns() {
    std::vector<SceneDebrisSpawn> result = std::move(debrisSpawns_);
    debrisSpawns_.clear();
    return result;
}

std::vector<SceneFireSpawn> Scene::ConsumeFireSpawns() {
    std::vector<SceneFireSpawn> result = std::move(fireSpawns_);
    fireSpawns_.clear();
    return result;
}

SceneStats Scene::Stats() const {
    SceneStats stats;
    stats.chunks = 0;
    for (const SceneObject& object : objects_) {
        switch (object.type) {
        case SceneObjectType::Building:
            ++stats.buildings;
            break;
        case SceneObjectType::Vehicle:
            ++stats.vehicles;
            break;
        case SceneObjectType::Tree:
            ++stats.trees;
            break;
        default:
            break;
        }
        stats.chunks += int(object.chunks.size());
    }
    stats.bodies = physics_.BodyCount();
    stats.staticBodies = 0;
    for (const RigidBody& body : physics_.Bodies()) {
        if (body.isStatic) {
            ++stats.staticBodies;
        }
    }
    return stats;
}

} // namespace ex::scene
