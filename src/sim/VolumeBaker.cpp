#include "sim/VolumeBaker.h"

#include "core/Log.h"
#include "core/ThreadPool.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

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
inline float ValueNoise(float x, float y, float z) {    const int xi = int(std::floor(x));
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

inline uint16_t ToHalf(float value) {
    const DirectX::PackedVector::HALF half = DirectX::PackedVector::XMConvertFloatToHalf(value);
    uint16_t bits = 0;
    std::memcpy(&bits, &half, sizeof(bits));
    return bits;
}

} // namespace

void VolumeBaker::Initialize(core::ThreadPool* pool, int resolution) {
    pool_ = pool;
    resolution_ = std::clamp(resolution, 16, 1024);
    Reset();
}

void VolumeBaker::Reset() {
    WaitForIdle();
    ready_.store(false, std::memory_order_release);
    inFlight_.store(false, std::memory_order_release);
    uploadedVersion_.store(0, std::memory_order_release);
    hasData_ = false;
    autoExtent_ = 0.0;
    autoTop_ = 0.0;
    frame_ = VolumeFrame{};
}

void VolumeBaker::SetAutoScale(bool enabled) {
    if (autoScale_ == enabled) {
        return;
    }
    autoScale_ = enabled;
    autoExtent_ = 0.0;
    autoTop_ = 0.0;
}

void VolumeBaker::SetResolution(int resolution) {
    resolution = std::clamp(resolution, 16, 1024);
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
        return; // A finished frame is still waiting to be uploaded.
    }
    if (frame_.version != uploadedVersion_.load(std::memory_order_acquire)) {
        return; // The previous frame has not been uploaded yet.
    }

    const BlastState& state = model.State();
    const double height = model.GetCharge().heightOfBurstM;
    const double fireballRadius = std::max(state.fireballRadius, 0.05);
    const double cloudRadius = std::max(state.cloudRadius, 0.05);

    const float centerX = float(state.cloudDriftX * 0.45);
    const float centerZ = float(state.cloudDriftZ * 0.45);

    // Volume bounds. In auto-scale mode the box always contains the fireball,
    // the smoke envelope, the cap and the base dust with a safety margin. It
    // only grows, and the fixed voxel count means the effective accuracy drops
    // by the same factor as the box grows.
    double halfExtent = 0.0;
    double top = 0.0;
    if (autoScale_) {
        const double drift = std::max(std::abs(state.cloudDriftX), std::abs(state.cloudDriftZ)) * 0.45;
        const double smokeRadius = fireballRadius * 1.75;
        const double baseDustRadius = fireballRadius * 2.4;
        const double horizontal =
            std::max({smokeRadius + drift, cloudRadius * 1.35 + drift, baseDustRadius, 0.2});
        // Tighter than BlastModel::CloudTopHeightM (2.2 R): the box only has to
        // contain the baked field, not provide a display margin.
        const double requiredTop = std::max({state.fireballCenterY + fireballRadius * 2.0,
                                             state.cloudCenterY + cloudRadius * 1.6,
                                             height + fireballRadius * 2.2, 0.6});

        const double requiredExtent = horizontal * 1.10;
        const double requiredTopMargin = requiredTop * 1.05;

        if (autoExtent_ < 0.05) {
            autoExtent_ = requiredExtent;
        } else if (requiredExtent > autoExtent_ * 1.02) {
            autoExtent_ = requiredExtent * 1.15;
        }
        if (autoTop_ < 0.05) {
            autoTop_ = requiredTopMargin;
        } else if (requiredTopMargin > autoTop_ * 1.02) {
            autoTop_ = requiredTopMargin * 1.15;
        }

        halfExtent = autoExtent_;
        top = autoTop_;
    } else {
        // Fixed box: sized for the maximum fireball of the charge. Smoke and
        // cloud can leave the box, which is useful for stable-resolution
        // comparisons.
        const double maxFireball = std::max(model.FireballMaxRadiusM(), 0.1);
        halfExtent = std::max(maxFireball * 3.2, 0.5);
        top = std::max(height + maxFireball * 3.2, 1.0);
    }

    VolumeBounds bounds{};
    bounds.min = XMFLOAT3(centerX - float(halfExtent), 0.0f, centerZ - float(halfExtent));
    bounds.max = XMFLOAT3(centerX + float(halfExtent), float(top), centerZ + float(halfExtent));

    const int resolution = resolution_;
    const float voxelSize = float(2.0 * halfExtent / double(resolution));

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
    params.extinctionScale = VolumeExtinctionScale(fireballRadius);
    params.voxelSize = voxelSize;

    // The cloud emerges from the cooling fireball.
    const float capRamp = Saturate((float(state.time) - 0.18f) / 1.2f);
    const float capSize = Saturate(float(cloudRadius) /
                                   std::max(float(fireballRadius) * 1.35f, 0.05f));
    params.cloudStrength = capRamp * capSize;

    job_.params = params;
    job_.bounds = bounds;
    job_.extinctionScale = params.extinctionScale;
    job_.resolution = resolution;

    frame_.resolution = resolution;
    frame_.bounds = bounds;
    frame_.extinctionScale = params.extinctionScale;
    // 16-bit half floats up to 512³. 1024³ would exceed the 4 GiB D3D11
    // resource limit as RGBA8, so it is split into two RG8 textures (2 GiB
    // each): RG = density/temperature, RG = dust/soot.
    frame_.split = resolution >= 1024;
    frame_.halfFloat = resolution < 1024;
    frame_.bytesPerVoxel = frame_.split ? 2 : (frame_.halfFloat ? 8 : 4);
    job_.bytesPerVoxel = frame_.bytesPerVoxel;
    job_.halfFloat = frame_.halfFloat;
    job_.split = frame_.split;

    const size_t voxelCount = size_t(resolution) * size_t(resolution) * size_t(resolution);
    try {
        frame_.rgba.resize(voxelCount * size_t(frame_.bytesPerVoxel));
        if (frame_.split) {
            frame_.rgba2.resize(voxelCount * 2);
        } else {
            frame_.rgba2.clear();
        }
    } catch (const std::bad_alloc&) {
        LogError("Volume bake: out of memory for %d^3 (%.2f GB).", resolution,
                 double(voxelCount) * double(frame_.bytesPerVoxel) *
                     (frame_.split ? 2.0 : 1.0) / (1024.0 * 1024.0 * 1024.0));
        return;
    }
    job_.target = &frame_.rgba;
    job_.target2 = frame_.split ? &frame_.rgba2 : nullptr;

    const uint64_t nextVersion = frame_.version + 1;
    inFlight_.store(true, std::memory_order_release);
    ready_.store(false, std::memory_order_release);

    const auto start = std::chrono::high_resolution_clock::now();
    const int sliceChunk = std::max(1, resolution / (int(pool_->WorkerCount()) * 4));
    BakeJob* job = &job_;

    auto finish = [this, start, nextVersion] {
        const auto end = std::chrono::high_resolution_clock::now();
        lastBakeMs_ = std::chrono::duration<float, std::milli>(end - start).count();
        frame_.version = nextVersion;
        hasData_ = true;
        inFlight_.store(false, std::memory_order_release);
        ready_.store(true, std::memory_order_release);
    };

    if (synchronous) {
        pool_->ParallelFor(resolution, sliceChunk, [this, job](int begin, int end) {
            BakeRange(begin, end, *job);
        });
        finish();
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
    return ready_.load(std::memory_order_acquire);
}

bool VolumeBaker::HasNewFrame() const {
    return ready_.load(std::memory_order_acquire) &&
           frame_.version != uploadedVersion_.load(std::memory_order_acquire);
}

bool VolumeBaker::CanStartBake() const {
    return !inFlight_.load(std::memory_order_acquire) &&
           !ready_.load(std::memory_order_acquire) &&
           frame_.version == uploadedVersion_.load(std::memory_order_acquire);
}

void VolumeBaker::MarkUploaded(uint64_t version) {
    uploadedVersion_.store(version, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
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

    std::vector<uint8_t>& target = *job.target;
    std::vector<uint8_t>* target2 = job.target2;
    const int bytesPerVoxel = job.bytesPerVoxel;

    for (int z = zBegin; z < zEnd; ++z) {
        const float worldZ = minZ + (float(z) + 0.5f) * sizeZ * invResolution;
        uint8_t* rowZ = target.data() + size_t(z) * size_t(resolution) * size_t(resolution) *
                                            size_t(bytesPerVoxel);
        uint8_t* rowZ2 = target2 ? target2->data() + size_t(z) * size_t(resolution) *
                                                       size_t(resolution) * 2
                                 : nullptr;

        for (int y = 0; y < resolution; ++y) {
            const float worldY = minY + (float(y) + 0.5f) * sizeY * invResolution;
            uint8_t* rowY = rowZ + size_t(y) * size_t(resolution) * size_t(bytesPerVoxel);
            uint8_t* rowY2 = rowZ2 ? rowZ2 + size_t(y) * size_t(resolution) * 2 : nullptr;

            for (int x = 0; x < resolution; ++x) {
                const float worldX = minX + (float(x) + 0.5f) * sizeX * invResolution;

                float density = 0.0f;
                float temperature = ambient;
                float dust = 0.0f;
                float soot = 0.0f;
                float fireballDistance = 0.0f;

                // --- Fireball ----------------------------------------------
                if (p.fireballVisible) {
                    const float dx = worldX - p.fireballX;
                    const float dy = worldY - p.fireballY;
                    const float dz = worldZ - p.fireballZ;
                    const float distanceSq = dx * dx + dy * dy + dz * dz;
                    fireballDistance = std::sqrt(distanceSq);

                    const float noiseScale = 6.0f;
                    if (distanceSq < fireballRadiusSq * 2.25f) {

                        // Lumpy, filamentary surface. The lump amplitude is
                        // faded in with the fireball size so the initial
                        // explosion stays spherical instead of following the
                        // axis-aligned noise lattice.
                        const float lumpFactor =
                            SmoothStep(0.15f, 0.60f, fireballRadius);
                        const float lump =
                            0.62f * ValueNoise((worldX + 17.0f) * noiseScale,
                                               (worldY - 9.0f) * noiseScale,
                                               (worldZ + 31.0f) * noiseScale) +
                            0.38f * ValueNoise((worldX - 3.0f) * noiseScale * 2.4f,
                                               (worldY + 11.0f) * noiseScale * 2.4f,
                                               (worldZ - 19.0f) * noiseScale * 2.4f);
                        const float effectiveRadius =
                            fireballRadius *
                            (1.0f + (lump - 0.5f) * 0.70f * lumpFactor);
                        const float q = fireballDistance / std::max(effectiveRadius, 0.01f);
                        const float falloff = 1.0f - SmoothStep(0.25f, 1.20f, q);

                        if (falloff > 0.001f) {
                            // High-frequency detail: mottled temperature and
                            // soot, plus density holes that reveal the hot core.
                            const float detail = ValueNoise(
                                (worldX + 3.0f) * noiseScale * 2.2f,
                                (worldY - 13.0f) * noiseScale * 2.2f,
                                (worldZ + 29.0f) * noiseScale * 2.2f);

                            // Flame filaments: dense pockets with cooler gaps.
                            const float filament = ValueNoise(
                                (worldX + 41.0f) * noiseScale * 2.1f,
                                (worldY - 7.0f) * noiseScale * 2.1f,
                                (worldZ + 13.0f) * noiseScale * 2.1f);
                            const float flame = 0.60f + 0.85f * filament;
                            density += 2.4f * std::pow(falloff, 1.15f) * flame *
                                       (0.55f + 0.90f * detail);

                            // Hot core with a cooler, soot-rich surface.
                            const float heatNoise = ValueNoise(
                                (worldX - 5.0f) * noiseScale * 1.7f,
                                (worldY + 23.0f) * noiseScale * 1.7f,
                                (worldZ + 7.0f) * noiseScale * 1.7f);
                            const float core =
                                Saturate(1.0f - 0.9f * q) * (0.70f + 0.55f * heatNoise);
                            const float hot = ambient +
                                              (p.fireballTemperature - ambient) *
                                                  (0.28f + 1.00f * core) *
                                                  (0.72f + 0.55f * detail);
                            temperature = std::max(temperature, hot);

                            // Soot shell forms as the surface cools; noise
                            // breaks it into dark, uneven patches.
                            const float sootRamp = Saturate((p.time - 0.02f) / 0.25f);
                            const float shell = SmoothStep(0.52f, 0.98f, q);
                            soot = std::max(soot, shell * sootRamp * (0.45f + 0.75f * detail));
                        }
                    }

                    // --- Cooling smoke envelope ---------------------------
                    // A turbulent smoke shell grows around the fireball and
                    // progressively hides the glowing surface.
                    const float smokeRamp = Saturate((p.time - 0.035f) / 0.30f);
                    if (smokeRamp > 0.01f) {
                        const float radius = fireballRadius * (1.02f + 0.55f * smokeRamp);
                        const float q = fireballDistance / std::max(radius, 0.05f);
                        if (q < 2.0f) {
                            const float ring = std::exp(-std::pow((q - 0.95f) / 0.42f, 2.0f));
                            const float inner = std::exp(-std::pow(q / 0.95f, 2.0f));
                            const float turbulence =
                                0.55f + 0.85f * ValueNoise(worldX * noiseScale * 0.8f + 5.0f,
                                                           worldY * noiseScale * 0.8f - 3.0f,
                                                           worldZ * noiseScale * 0.8f + 9.0f);
                            const float shell = (ring + 0.85f * inner) * turbulence;
                            density += 2.20f * shell * smokeRamp;
                            const float smokeTemperature = ambient;
                            temperature = std::max(temperature, smokeTemperature);
                            soot = std::max(soot, 0.95f * shell * smokeRamp);
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
                        soot = std::max(soot, 0.65f * falloff * p.cloudStrength);
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
                            soot = std::max(soot, 0.70f * falloff * verticalMask);
                        }
                    }
                }

                // --- Base dust surge ---------------------------------------
                // The shock lifts a thick dust skirt around the fireball. It
                // rises, grows and increasingly obscures the lower fireball.
                if (worldY < fireballRadius * 3.0f) {
                        const float baseRamp = Saturate((p.time - 0.015f) / 0.15f);
                        if (baseRamp > 0.01f) {
                            const float radial = Length2D(worldX, worldZ);
                            const float baseRadius = fireballRadius * 1.9f;
                            const float ringOffset =
                                (radial - baseRadius * 0.55f) / (baseRadius * 0.65f);
                            const float ring = std::exp(-ringOffset * ringOffset);
                            const float height = std::exp(-worldY /
                                                          (fireballRadius * 0.80f + 0.25f));
                            const float turbulence =
                                0.55f + 0.80f * ValueNoise(worldX * 3.1f + 2.0f,
                                                           worldY * 3.1f - 5.0f,
                                                           worldZ * 3.1f + 7.0f);
                            density += 1.35f * ring * height * turbulence * baseRamp;
                            dust = std::max(dust, 0.95f * ring * height * baseRamp);
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

                uint8_t* out = rowY + size_t(x) * size_t(bytesPerVoxel);
                uint8_t* out2 = rowY2 ? rowY2 + size_t(x) * 2 : nullptr;
                if (density > 0.002f) {
                    const float values[4] = {density, Saturate(temperature / kTemperatureScale),
                                             Saturate(dust), Saturate(soot)};
                    if (job.split) {
                        out[0] = uint8_t(Saturate(values[0]) * 255.0f + 0.5f);
                        out[1] = uint8_t(Saturate(values[1]) * 255.0f + 0.5f);
                        out2[0] = uint8_t(Saturate(values[2]) * 255.0f + 0.5f);
                        out2[1] = uint8_t(Saturate(values[3]) * 255.0f + 0.5f);
                    } else {
                        for (int channel = 0; channel < 4; ++channel) {
                            const uint16_t half = ToHalf(values[channel]);
                            std::memcpy(out + channel * 2, &half, sizeof(half));
                        }
                    }
                } else {
                    std::memset(out, 0, size_t(bytesPerVoxel));
                    if (out2) {
                        out2[0] = 0;
                        out2[1] = 0;
                    }
                }
            }
        }
    }
}

} // namespace ex::sim
