#include "sim/VolumeBaker.h"

#include "core/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace DirectX;

namespace ex::sim {

namespace {

constexpr float kTemperatureScale = 12000.0f;

inline float Saturate(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float SmoothStep(float edge0, float edge1, float x) {
    const float t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

inline float Length2D(float x, float z) {
    return std::sqrt(x * x + z * z);
}

inline uint32_t HashCell(int x, int y, int z) {
    uint32_t hash = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^
                    uint32_t(z) * 0xcb1ab31fu;
    hash ^= hash >> 13;
    hash *= 0x5bd1e995u;
    hash ^= hash >> 15;
    return hash;
}

inline float HashCellFloat(int x, int y, int z) {
    return float(HashCell(x, y, z) & 0xFFFFFFu) / 16777215.0f;
}

// Trilinear value noise, used to break up the analytic fireball surface.
inline float ValueNoise(float x, float y, float z) {
    const int xi = int(std::floor(x));
    const int yi = int(std::floor(y));
    const int zi = int(std::floor(z));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    const float fz = z - float(zi);
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float uz = fz * fz * (3.0f - 2.0f * fz);

    const float n000 = HashCellFloat(xi, yi, zi);
    const float n100 = HashCellFloat(xi + 1, yi, zi);
    const float n010 = HashCellFloat(xi, yi + 1, zi);
    const float n110 = HashCellFloat(xi + 1, yi + 1, zi);
    const float n001 = HashCellFloat(xi, yi, zi + 1);
    const float n101 = HashCellFloat(xi + 1, yi, zi + 1);
    const float n011 = HashCellFloat(xi, yi + 1, zi + 1);
    const float n111 = HashCellFloat(xi + 1, yi + 1, zi + 1);

    const float nx00 = n000 + (n100 - n000) * ux;
    const float nx10 = n010 + (n110 - n010) * ux;
    const float nx01 = n001 + (n101 - n001) * ux;
    const float nx11 = n011 + (n111 - n011) * ux;
    const float nxy0 = nx00 + (nx10 - nx00) * uy;
    const float nxy1 = nx01 + (nx11 - nx01) * uy;
    return nxy0 + (nxy1 - nxy0) * uz;
}

} // namespace

void VolumeBaker::Initialize(core::ThreadPool* pool, int resolution) {
    pool_ = pool;
    resolution_ = std::max(16, resolution);
    Reset();
}

void VolumeBaker::Reset() {
    WaitForIdle();
    ready_.store(false, std::memory_order_release);
    hasData_ = false;
    version_ = 0;
    currentIndex_ = 0;
    writeIndex_ = 1;
    for (auto& buffer : buffers_) {
        buffer.resolution = 0;
        buffer.rgba.clear();
    }
}

void VolumeBaker::SetResolution(int resolution) {
    resolution = std::max(16, resolution);
    if (resolution == resolution_) {
        return;
    }
    Reset();
    resolution_ = resolution;
}

void VolumeBaker::Dispatch(const BlastModel& model, bool synchronous) {
    if (!pool_ || inFlight_.load(std::memory_order_acquire)) {
        return;
    }
    if (ready_.load(std::memory_order_acquire)) {
        // A finished frame is still waiting to be adopted; keep the newest one
        // for the next update instead of overwriting it.
        return;
    }

    const BlastState& state = model.State();
    const double height = model.GetCharge().heightOfBurstM;
    const double fireballRadius = std::max(state.fireballRadius, 0.05);
    const double cloudRadius = std::max(state.cloudRadius, 0.05);

    // World-space bounding box of the visible fireball / cloud, kept roughly
    // cubic so the volume texture has isotropic voxels.
    const double extent = std::max({fireballRadius * 2.2, cloudRadius * 1.8, 1.0});
    const double centerY = height + fireballRadius * 0.6;
    const double yMin = std::max(0.0, centerY - extent);
    const double top = std::max({yMin + 2.0 * extent, height + fireballRadius * 2.4,
                                 state.cloudCenterY + cloudRadius * 1.4, 1.5});

    const float centerX = float(state.cloudDriftX * 0.45);
    const float centerZ = float(state.cloudDriftZ * 0.45);

    VolumeBounds bounds{};
    bounds.min = XMFLOAT3(centerX - float(extent), float(yMin), centerZ - float(extent));
    bounds.max = XMFLOAT3(centerX + float(extent), float(top), centerZ + float(extent));

    const int resolution = resolution_;
    const float voxelSize = float(2.0 * extent / double(resolution));

    FieldParams params{};
    params.time = float(state.time);
    params.ambientTemperature = float(model.GetEnvironment().temperatureK);
    params.fireballX = 0.0f;
    params.fireballY = float(state.fireballCenterY);
    params.fireballZ = 0.0f;
    params.fireballRadius = float(fireballRadius);
    params.fireballTemperature = float(state.fireballTemperatureK);
    params.fireballVisible = state.fireballVisible;
    params.cloudX = float(state.cloudDriftX);
    params.cloudY = float(state.cloudCenterY);
    params.cloudZ = float(state.cloudDriftZ);
    params.cloudRadius = float(cloudRadius);
    params.cloudTemperature = float(state.cloudTemperatureK);
    params.stemRadius = float(std::max(state.stemRadius, 0.5));
    params.dustRingRadius = float(state.dustRingRadius);
    params.dustIntensity = float(state.dustIntensity);
    params.shockRadius = float(state.shockRadius);
    params.shockMach = float(state.shockMach);
    params.burstY = float(height);
    params.extinctionScale = float(1.2 / std::max(fireballRadius, 0.5));
    params.voxelSize = voxelSize;

    // The cloud emerges from the cooling fireball.
    const float capRamp = Saturate((float(state.time) - 0.18f) / 1.2f);
    const float capSize = Saturate(float(cloudRadius) /
                                   std::max(float(fireballRadius) * 1.35f, 0.05f));
    params.cloudStrength = capRamp * capSize;

    // Make sure the mesh contains the stem / ground interaction region.
    params.cloudStrength = std::max(params.cloudStrength, 0.0f);

    job_.params = params;
    job_.bounds = bounds;
    job_.extinctionScale = params.extinctionScale;
    job_.resolution = resolution;

    VolumeFrame& target = buffers_[writeIndex_];
    target.resolution = resolution;
    target.bounds = bounds;
    target.extinctionScale = params.extinctionScale;
    const size_t voxelCount = size_t(resolution) * size_t(resolution) * size_t(resolution);
    target.rgba.resize(voxelCount * 4);
    job_.target = &target.rgba;

    inFlight_.store(true, std::memory_order_release);
    ready_.store(false, std::memory_order_release);

    const auto start = std::chrono::high_resolution_clock::now();
    const int sliceChunk = std::max(1, resolution / (int(pool_->WorkerCount()) * 4));
    BakeJob* job = &job_;

    auto finish = [this, start] {
        const auto end = std::chrono::high_resolution_clock::now();
        lastBakeMs_ = std::chrono::duration<float, std::milli>(end - start).count();
        inFlight_.store(false, std::memory_order_release);
        ready_.store(true, std::memory_order_release);
    };

    if (synchronous) {
        pool_->ParallelFor(resolution, sliceChunk, [this, job](int begin, int end) {
            BakeRange(begin, end, *job);
        });
        finish();
        AdoptFinished();
    } else {
        pool_->ParallelForAsync(
            resolution, sliceChunk,
            [this, job](int begin, int end) { BakeRange(begin, end, *job); }, finish);
    }
}

void VolumeBaker::StartBake(const BlastModel& model) {
    Dispatch(model, false);
}

void VolumeBaker::BakeNow(const BlastModel& model) {
    Dispatch(model, true);
}

bool VolumeBaker::Poll() {
    if (!ready_.load(std::memory_order_acquire)) {
        return false;
    }
    AdoptFinished();
    return hasData_;
}

void VolumeBaker::AdoptFinished() {
    if (!ready_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    currentIndex_ = writeIndex_;
    writeIndex_ = 1 - currentIndex_;
    hasData_ = true;
    ++version_;
}

void VolumeBaker::WaitForIdle() {
    while (inFlight_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void VolumeBaker::BakeRange(int zBegin, int zEnd, const BakeJob& job) {
    const int resolution = job.resolution;
    const float invResolution = 1.0f / float(resolution);
    const VolumeBounds& bounds = job.bounds;
    const FieldParams& p = job.params;

    const float minX = bounds.min.x;
    const float minY = bounds.min.y;
    const float minZ = bounds.min.z;
    const float sizeX = bounds.max.x - bounds.min.x;
    const float sizeY = bounds.max.y - bounds.min.y;
    const float sizeZ = bounds.max.z - bounds.min.z;

    const float ambient = p.ambientTemperature;
    const float fireballRadius = std::max(p.fireballRadius, 0.25f);
    const float fireballRadiusSq = fireballRadius * fireballRadius;
    const float cloudRadius = std::max(p.cloudRadius, 0.5f);
    const float capMajor = cloudRadius * 0.62f;
    const float capMinor = std::max(cloudRadius * 0.52f, 0.75f);
    const float capY = p.cloudY;
    const float capX = p.cloudX;
    const float capZ = p.cloudZ;

    std::vector<float>& target = *job.target;

    for (int z = zBegin; z < zEnd; ++z) {
        const float worldZ = minZ + (float(z) + 0.5f) * sizeZ * invResolution;
        float* rowZ = target.data() + size_t(z) * size_t(resolution) * size_t(resolution) * 4;

        for (int y = 0; y < resolution; ++y) {
            const float worldY = minY + (float(y) + 0.5f) * sizeY * invResolution;
            float* rowY = rowZ + size_t(y) * size_t(resolution) * 4;

            for (int x = 0; x < resolution; ++x) {
                const float worldX = minX + (float(x) + 0.5f) * sizeX * invResolution;

                float density = 0.0f;
                float temperature = ambient;
                float dust = 0.0f;

                // --- Fireball ----------------------------------------------
                if (p.fireballVisible) {
                    const float dx = worldX - p.fireballX;
                    const float dy = worldY - p.fireballY;
                    const float dz = worldZ - p.fireballZ;
                    const float distanceSq = dx * dx + dy * dy + dz * dz;

                    if (distanceSq < fireballRadiusSq * 2.25f) {
                        const float distance = std::sqrt(distanceSq);
                        const float noiseScale = 2.6f / fireballRadius;

                        // Lumpy surface: perturb the effective radius with a
                        // 3D noise field.
                        const float lump = ValueNoise(
                            (worldX + 17.0f) * noiseScale, (worldY - 9.0f) * noiseScale,
                            (worldZ + 31.0f) * noiseScale);
                        const float effectiveRadius =
                            fireballRadius * (0.80f + 0.42f * lump);
                        const float q = distance / std::max(effectiveRadius, 0.01f);
                        const float falloff = 1.0f - SmoothStep(0.55f, 1.18f, q);

                        if (falloff > 0.001f) {
                            density += 2.0f * std::pow(falloff, 1.15f);

                            // Filamentary temperature structure.
                            const float heatNoise = ValueNoise(
                                (worldX - 5.0f) * noiseScale * 1.9f,
                                (worldY + 23.0f) * noiseScale * 1.9f,
                                (worldZ + 7.0f) * noiseScale * 1.9f);
                            const float core =
                                falloff * (0.65f + 0.70f * heatNoise);
                            const float hot = (0.22f + 0.62f * core) *
                                                  (p.fireballTemperature - ambient) +
                                              ambient;
                            temperature = std::max(temperature, hot);
                        }
                    }
                }

                // --- Shock front (compressed, heated air) ------------------
                if (p.shockRadius > 0.1f && p.shockMach > 1.02f) {
                    const float dx = worldX;
                    const float dy = worldY - p.burstY;
                    const float dz = worldZ;
                    const float distance =
                        std::sqrt(dx * dx + dy * dy + dz * dz) - p.shockRadius;
                    const float thickness =
                        std::max(p.voxelSize * 1.8f, p.shockRadius * 0.035f);

                    if (std::abs(distance) < thickness * 2.5f) {
                        const float q = distance / thickness;
                        const float shell = std::exp(-q * q);
                        density += 0.65f * shell;

                        const float mach = std::max(p.shockMach, 1.0f);
                        const float machSq = mach * mach;
                        const float temperatureRatio =
                            ((2.8f * machSq - 0.4f) * (0.4f * machSq + 2.0f)) /
                            (5.76f * machSq);
                        const float shockTemperature =
                            std::min(ambient * std::max(temperatureRatio, 1.0f), 6500.0f);
                        temperature = std::max(
                            temperature, ambient + (shockTemperature - ambient) * shell);
                        dust = std::max(dust, 0.35f * shell);
                    }
                }

                // --- Rising mushroom cap (torus) ---------------------------
                if (p.cloudStrength > 0.01f) {
                    const float dx = worldX - capX;
                    const float dz = worldZ - capZ;
                    const float horiz = Length2D(dx, dz);
                    const float ringOffset = horiz - capMajor;
                    const float dy = (worldY - capY) * 1.35f;
                    const float torusDistance = std::sqrt(ringOffset * ringOffset + dy * dy) / capMinor;

                    if (torusDistance < 1.8f) {
                        const float falloff = std::exp(-torusDistance * torusDistance * 1.25f);
                        density += 1.1f * falloff * p.cloudStrength;
                        const float hot = ambient + (p.cloudTemperature - ambient) *
                                                         (0.35f + 0.65f * falloff);
                        temperature = std::max(temperature, hot);
                        dust = std::max(dust, 0.25f * falloff);
                    }

                    // --- Stem ----------------------------------------------
                    if (worldY > 0.0f && worldY < capY + capMinor) {
                        const float axisX = capX * 0.65f;
                        const float axisZ = capZ * 0.65f;
                        const float wobbleX =
                            std::sin(worldY * 0.035f + p.time * 0.6f) * p.stemRadius * 0.55f;
                        const float wobbleZ =
                            std::cos(worldY * 0.028f + p.time * 0.45f) * p.stemRadius * 0.55f;

                        const float stemRadius =
                            p.stemRadius * (0.85f + 0.55f * worldY / std::max(capY, 1.0f));
                        const float radial =
                            Length2D(worldX - axisX - wobbleX, worldZ - axisZ - wobbleZ) /
                            std::max(stemRadius, 0.5f);

                        if (radial < 2.5f) {
                            const float verticalMask =
                                SmoothStep(0.0f, 1.0f, worldY / 1.5f) *
                                (1.0f - SmoothStep(capY * 0.9f, capY + capMinor * 0.6f, worldY));
                            const float falloff = std::exp(-radial * radial * 1.5f);
                            density += 0.65f * falloff * verticalMask * p.cloudStrength;
                            temperature = std::max(
                                temperature,
                                ambient + (p.cloudTemperature - ambient) * 0.55f * falloff * verticalMask);
                            dust = std::max(dust, 0.55f * falloff * verticalMask);
                        }
                    }
                }

                // --- Ground dust ring (only the part inside the box) ------
                if (p.dustIntensity > 0.01f && p.dustRingRadius > 1.0f && worldY < 12.0f) {
                    const float horiz = Length2D(worldX, worldZ);
                    const float width = std::max(p.dustRingRadius * 0.10f, 2.0f);
                    const float ringOffset = (horiz - p.dustRingRadius) / width;
                    if (std::abs(ringOffset) < 3.0f) {
                        const float ring = std::exp(-ringOffset * ringOffset);
                        const float heightFalloff =
                            std::exp(-worldY / (p.dustRingRadius * 0.06f + 1.5f));
                        density += 0.9f * ring * heightFalloff * p.dustIntensity;
                        dust = std::max(dust, 0.85f);
                    }
                }

                float* out = rowY + size_t(x) * 4;
                if (density > 0.002f) {
                    out[0] = density;
                    out[1] = Saturate(temperature / kTemperatureScale);
                    out[2] = dust;
                    out[3] = 1.0f;
                } else {
                    out[0] = 0.0f;
                    out[1] = 0.0f;
                    out[2] = 0.0f;
                    out[3] = 1.0f;
                }
            }
        }
    }
}

} // namespace ex::sim
