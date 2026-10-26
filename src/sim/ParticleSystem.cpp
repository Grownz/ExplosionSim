#include "sim/ParticleSystem.h"

#include "core/ThreadPool.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace ex::sim {

namespace {

constexpr float kPi = 3.14159265358979f;

float Length(const XMFLOAT3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

XMFLOAT3 Scale(const XMFLOAT3& v, float s) {
    return XMFLOAT3(v.x * s, v.y * s, v.z * s);
}

XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}

XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}

// Cheap pseudo-noise used to break up the motion of smoke puffs.
float HashNoise(float x, float y, float z) {
    const float value = std::sin(x * 12.9898f + y * 78.233f + z * 37.719f) * 43758.5453f;
    return value - std::floor(value);
}

// Approximate blackbody colour (linear RGB), valid for 1000 K .. 40000 K.
// Particle sprites use this Tanner-Helland approximation; the volumetric path
// uses the Kim et al. fit in common.hlsli.
XMFLOAT3 BlackbodyColor(float kelvin) {
    const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (t <= 66.0f) {
        r = 1.0f;
        g = std::clamp((99.4708025861f * std::log(t) - 161.1195681661f) / 255.0f, 0.0f, 1.0f);
        b = t <= 19.0f ? 0.0f
                       : std::clamp((std::log(t - 10.0f) * 138.5177312231f - 305.0447927307f) / 255.0f,
                                    0.0f, 1.0f);
    } else {
        r = std::clamp(329.698727446f * std::pow(t - 60.0f, -0.1332047592f) / 255.0f, 0.0f, 1.0f);
        g = std::clamp(288.1221695283f * std::pow(t - 60.0f, -0.0755148492f) / 255.0f, 0.0f, 1.0f);
        b = 1.0f;
    }

    // Convert the sRGB approximation into linear space.
    auto toLinear = [](float c) { return std::pow(c, 2.2f); };
    return XMFLOAT3(toLinear(r), toLinear(g), toLinear(b));
}

} // namespace

void ParticleSystem::Initialize(int capacity) {
    capacity = std::max(1024, capacity);
    particles_.assign(size_t(capacity), Particle{});

    // Species ranges. The split favours the visually dominant fire and smoke.
    const int debris = int(capacity * 0.05f);
    const int sparks = int(capacity * 0.12f);
    const int fire = int(capacity * 0.26f);
    const int smoke = int(capacity * 0.31f);
    const int dust = capacity - debris - sparks - fire - smoke;

    rangeBegin_[int(ParticleKind::Debris)] = 0;
    rangeEnd_[int(ParticleKind::Debris)] = debris;
    rangeBegin_[int(ParticleKind::Spark)] = debris;
    rangeEnd_[int(ParticleKind::Spark)] = debris + sparks;
    rangeBegin_[int(ParticleKind::Fire)] = debris + sparks;
    rangeEnd_[int(ParticleKind::Fire)] = debris + sparks + fire;
    rangeBegin_[int(ParticleKind::Smoke)] = debris + sparks + fire;
    rangeEnd_[int(ParticleKind::Smoke)] = debris + sparks + fire + smoke;
    rangeBegin_[int(ParticleKind::Dust)] = debris + sparks + fire + smoke;
    rangeEnd_[int(ParticleKind::Dust)] = rangeBegin_[int(ParticleKind::Dust)] + dust;

    additive_.reserve(size_t(capacity));
    alpha_.reserve(size_t(capacity));
    fire_.reserve(size_t(capacity));

    Reset();
}

void ParticleSystem::Reset() {
    for (auto& particle : particles_) {
        particle.alive = false;
    }
    additive_.clear();
    alpha_.clear();
    fire_.clear();
    aliveCount_ = 0;
    smokeSpawnAccumulator_ = 0.0f;
    dustSpawnAccumulator_ = 0.0f;
    for (int i = 0; i < int(ParticleKind::Count); ++i) {
        aliveByKind_[i] = 0;
        // The cursor is an offset inside the species range, not an absolute
        // index (AcquireSlot adds rangeBegin_ itself).
        cursor_[i] = 0;
    }
    rng_.Seed(0xC0FFEEull);
    detonationTime_ = 0.0f;
}

int ParticleSystem::AcquireSlot(ParticleKind kind) {
    const int index = int(kind);
    const int begin = rangeBegin_[index];
    const int end = rangeEnd_[index];
    if (end <= begin) {
        return -1;
    }

    // Round-robin within the species range. Recycles the oldest particle when
    // the range is exhausted.
    for (int attempt = 0; attempt < end - begin; ++attempt) {
        const int slot = begin + cursor_[index];
        cursor_[index] = (cursor_[index] + 1) % (end - begin);
        if (!particles_[slot].alive) {
            return slot;
        }
    }
    const int slot = begin + cursor_[index];
    cursor_[index] = (cursor_[index] + 1) % (end - begin);
    return slot;
}

void ParticleSystem::Detonate(const BlastModel& model) {
    const BlastState& state = model.State();
    detonationTime_ = float(state.time);

    // Scale the effect with the charge: a 200 g charge produces a ~1.7 m
    // fireball, a 20 t charge a ~10.5 m one.
    sizeScale_ = float(std::clamp(model.FireballMaxRadiusM() / 10.45, 0.05, 1.0));
    const float massRatio = float(model.GetCharge().tntMassKg / 20000.0);
    countScale_ = std::clamp(std::pow(massRatio, 0.35f), 0.25f, 1.0f);

    SpawnDebris(model, int(float(rangeEnd_[int(ParticleKind::Debris)]) * 0.85f * countScale_));
    SpawnSparks(model, int(float(rangeEnd_[int(ParticleKind::Spark)]) * 0.75f * countScale_));
    SpawnBaseDust(model, int(500.0f * countScale_));
    SpawnFire(model, int(float(rangeEnd_[int(ParticleKind::Fire)]) * 0.10f * countScale_));
}

void ParticleSystem::SpawnDebris(const BlastModel& model, int count) {
    const BlastState& state = model.State();
    const Environment& env = model.GetEnvironment();
    const XMFLOAT3 center{0.0f, float(model.GetCharge().heightOfBurstM), 0.0f};
    const float burstRadius = std::max(float(state.fireballRadius) * 0.35f, 0.2f);

    for (int i = 0; i < count; ++i) {
        const int slot = AcquireSlot(ParticleKind::Debris);
        if (slot < 0) {
            return;
        }
        Particle& p = particles_[slot];
        const XMFLOAT3 dir = rng_.UnitVector();
        const float speed = rng_.Range(180.0f, 950.0f);
        const float offset = rng_.Range(0.2f, 1.0f) * burstRadius;

        p.position = Add(center, Scale(dir, offset));
        p.velocity = Scale(dir, speed);
        p.velocity.y += rng_.Range(20.0f, 120.0f);
        p.size = rng_.Range(0.35f, 2.2f) * sizeScale_ * 0.45f;
        p.life = 0.0f;
        p.maxLife = rng_.Range(6.0f, 18.0f);
        p.temperature = rng_.Range(1400.0f, 2100.0f);
        p.opacity = 1.0f;
        p.growth = 0.0f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Debris;
        p.alive = true;

        // 0.5 * rho * Cd * A / m, with m derived from a dense fragment.
        const float mass = 2600.0f * (4.0f / 3.0f) * kPi * p.size * p.size * p.size;
        const float area = kPi * p.size * p.size;
        p.drag = 0.5f * float(env.AirDensity()) * 0.85f * area / std::max(mass, 1e-3f);
    }
}

void ParticleSystem::SpawnBaseDust(const BlastModel& model, int count) {
    const BlastState& state = model.State();
    const float burstHeight = float(model.GetCharge().heightOfBurstM);
    const float baseRadius = std::max(float(state.fireballRadius) * 1.6f, 0.35f);

    for (int i = 0; i < count; ++i) {
        const int slot = AcquireSlot(ParticleKind::Dust);
        if (slot < 0) {
            return;
        }
        Particle& p = particles_[slot];
        const float angle = rng_.Float01() * 2.0f * kPi;
        const float radius = baseRadius * rng_.Range(0.4f, 1.6f);

        p.position.x = std::cos(angle) * radius;
        p.position.y = burstHeight * rng_.Range(0.05f, 0.8f);
        p.position.z = std::sin(angle) * radius;
        p.velocity.x = std::cos(angle) * rng_.Range(1.0f, 7.0f);
        p.velocity.z = std::sin(angle) * rng_.Range(1.0f, 7.0f);
        p.velocity.y = rng_.Range(1.0f, 5.0f);

        p.size = rng_.Range(0.4f, 1.6f) * sizeScale_;
        p.life = 0.0f;
        p.maxLife = rng_.Range(3.5f, 8.0f);
        p.temperature = rng_.Range(350.0f, 700.0f);
        p.opacity = 0.0f;
        p.growth = rng_.Range(0.5f, 1.6f) * sizeScale_;
        p.drag = 0.0f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Dust;
        p.alive = true;
    }
}

void ParticleSystem::SpawnFire(const BlastModel& model, int count) {
    const BlastState& state = model.State();
    const XMFLOAT3 center{0.0f, float(state.fireballCenterY), 0.0f};
    const float radius = std::max(float(state.fireballRadius), 0.1f);
    const float particleScale = std::clamp(radius / 0.6f, 0.35f, 3.0f);

    for (int i = 0; i < count; ++i) {
        const int slot = AcquireSlot(ParticleKind::Fire);
        if (slot < 0) {
            return;
        }
        Particle& p = particles_[slot];
        const XMFLOAT3 dir = rng_.UnitVector();
        const float offset = radius * rng_.Range(0.55f, 1.05f);

        p.position = Add(center, Scale(dir, offset));
        p.velocity = Scale(dir, rng_.Range(1.5f, 9.0f));
        p.velocity.y += rng_.Range(1.0f, 6.0f);
        p.size = rng_.Range(0.18f, 0.55f) * particleScale;
        p.life = 0.0f;
        p.maxLife = rng_.Range(0.20f, 0.70f);
        p.temperature = rng_.Range(2400.0f, 4000.0f);
        p.opacity = 1.0f;
        p.growth = rng_.Range(0.5f, 1.4f) * particleScale;
        p.drag = 1.2f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Fire;
        p.alive = true;
    }
}

void ParticleSystem::SpawnSparks(const BlastModel& model, int count) {    const BlastState& state = model.State();
    const Environment& env = model.GetEnvironment();
    const XMFLOAT3 center{0.0f, float(model.GetCharge().heightOfBurstM), 0.0f};
    const float burstRadius = std::max(float(state.fireballRadius) * 0.25f, 0.1f);

    for (int i = 0; i < count; ++i) {
        const int slot = AcquireSlot(ParticleKind::Spark);
        if (slot < 0) {
            return;
        }
        Particle& p = particles_[slot];
        const XMFLOAT3 dir = rng_.UnitVector();
        const float speed = rng_.Range(80.0f, 720.0f);

        p.position = Add(center, Scale(dir, rng_.Float01() * burstRadius));
        p.velocity = Scale(dir, speed);
        p.velocity.y += rng_.Range(10.0f, 90.0f);
        p.size = rng_.Range(0.12f, 0.6f) * sizeScale_ * 0.55f;
        p.life = 0.0f;
        p.maxLife = rng_.Range(0.35f, 1.6f);
        p.temperature = rng_.Range(2200.0f, 4200.0f);
        p.opacity = 1.0f;
        p.growth = 0.0f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Spark;
        p.alive = true;

        const float mass = 0.4f * p.size * p.size * p.size;
        const float area = kPi * p.size * p.size;
        p.drag = 0.5f * float(env.AirDensity()) * 1.1f * area / std::max(mass, 1e-4f);
    }
}

void ParticleSystem::SpawnSceneDebris(const DirectX::XMFLOAT3& center, int count,
                                      float sizeScale, float speed) {
    for (int i = 0; i < count; ++i) {
        const int slot = AcquireSlot(ParticleKind::Debris);
        if (slot < 0) {
            return;
        }
        Particle& p = particles_[slot];
        const XMFLOAT3 direction = rng_.UnitVector();
        p.position = Add(center, Scale(direction, rng_.Range(0.1f, 0.9f)));
        p.velocity = Scale(direction, rng_.Range(0.4f, 1.0f) * speed);
        p.velocity.y += rng_.Range(2.0f, 8.0f);
        p.size = rng_.Range(0.15f, 0.45f) * std::max(sizeScale, 0.1f);
        p.life = 0.0f;
        p.maxLife = rng_.Range(3.0f, 8.0f);
        p.temperature = rng_.Range(600.0f, 1400.0f);
        p.opacity = 1.0f;
        p.growth = 0.0f;
        p.drag = 0.15f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Debris;
        p.alive = true;
    }
}

void ParticleSystem::SpawnSceneFire(const DirectX::XMFLOAT3& center, int count,
                                    float scale) {
    for (int i = 0; i < count; ++i) {
        const int slot = AcquireSlot(ParticleKind::Fire);
        if (slot < 0) {
            return;
        }
        Particle& p = particles_[slot];
        const XMFLOAT3 direction = rng_.UnitVector();
        p.position = Add(center, Scale(direction, rng_.Range(0.2f, 1.2f) * scale));
        p.velocity = Scale(direction, rng_.Range(0.5f, 2.0f) * scale);
        p.velocity.y += rng_.Range(1.5f, 4.0f);
        p.size = rng_.Range(0.9f, 2.0f) * scale;
        p.life = 0.0f;
        p.maxLife = rng_.Range(0.5f, 1.2f);
        p.temperature = rng_.Range(1600.0f, 2600.0f);
        p.opacity = 1.0f;
        p.growth = rng_.Range(0.6f, 1.4f) * scale;
        p.drag = 1.2f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Fire;
        p.alive = true;
    }
}

void ParticleSystem::UpdateSpawners(float dt, const BlastModel& model) {
    const BlastState& state = model.State();
    const float time = float(state.time);
    const XMFLOAT3 burst{0.0f, float(model.GetCharge().heightOfBurstM), 0.0f};

    // Smoke billows follow the fireball during formation and then the rising
    // mushroom cap. The spawn rate fades as the cloud cools.
    const float smokeRate = 900.0f * countScale_ * std::exp(-time / 2.5f);
    smokeSpawnAccumulator_ += smokeRate * dt;
    int smokeCount = int(smokeSpawnAccumulator_);
    smokeSpawnAccumulator_ -= float(smokeCount);

    const float smokeRadiusScale = std::max(float(state.cloudRadius), 1.0f);
    for (int i = 0; i < smokeCount; ++i) {
        const int slot = AcquireSlot(ParticleKind::Smoke);
        if (slot < 0) {
            break;
        }
        Particle& p = particles_[slot];

        const float ringAngle = rng_.Float01() * 2.0f * kPi;
        const float ringRadius = std::max(float(state.fireballRadius) * 0.8f, 0.5f);
        const float height = float(state.fireballCenterY) + rng_.Range(-0.3f, 0.9f) * float(state.fireballRadius);
        const float jitter = rng_.Range(-0.25f, 0.25f) * float(state.fireballRadius);

        const XMFLOAT3 centerOnAxis{float(state.cloudDriftX), height, float(state.cloudDriftZ)};
        p.position.x = centerOnAxis.x + std::cos(ringAngle) * ringRadius + jitter;
        p.position.y = centerOnAxis.y + rng_.Range(-0.2f, 0.4f) * float(state.fireballRadius);
        p.position.z = centerOnAxis.z + std::sin(ringAngle) * ringRadius + jitter;

        // Toroidal updraft plus outward expansion.
        p.velocity.x = std::cos(ringAngle) * rng_.Range(6.0f, 26.0f);
        p.velocity.z = std::sin(ringAngle) * rng_.Range(6.0f, 26.0f);
        p.velocity.y = rng_.Range(4.0f, 22.0f) + float(state.cloudRiseSpeed) * 0.35f;

        p.size = rng_.Range(0.8f, 2.6f) * std::min(2.5f, smokeRadiusScale * 0.12f + 1.0f) * sizeScale_ * 0.8f;
        p.life = 0.0f;
        p.maxLife = rng_.Range(7.0f, 16.0f);
        p.temperature = rng_.Range(350.0f, 650.0f);
        p.opacity = 0.0f;
        p.growth = rng_.Range(0.6f, 2.4f) * sizeScale_;
        p.drag = 0.0f;
        p.seed = rng_.Float01() * 1000.0f;
        p.kind = ParticleKind::Smoke;
        p.alive = true;
    }

    // Ground dust is lifted by the expanding shock front.
    if (state.dustRingRadius > 1.0) {
        const float dustRate = 900.0f * float(state.dustIntensity) + 40.0f * std::exp(-time / 4.0f);
        dustSpawnAccumulator_ += dustRate * dt;
        int dustCount = int(dustSpawnAccumulator_);
        dustSpawnAccumulator_ -= float(dustCount);

        const float ring = float(state.dustRingRadius);
        for (int i = 0; i < dustCount; ++i) {
            const int slot = AcquireSlot(ParticleKind::Dust);
            if (slot < 0) {
                break;
            }
            Particle& p = particles_[slot];
            const float angle = rng_.Float01() * 2.0f * kPi;
            const float radius = ring * rng_.Range(0.75f, 1.02f);
            const float outward = rng_.Range(8.0f, 34.0f);

            p.position.x = std::cos(angle) * radius + rng_.Range(-1.0f, 1.0f) * ring * 0.05f;
            p.position.y = rng_.Range(0.4f, 2.5f);
            p.position.z = std::sin(angle) * radius;
            p.velocity.x = std::cos(angle) * outward;
            p.velocity.z = std::sin(angle) * outward;
            p.velocity.y = rng_.Range(2.0f, 11.0f);

            p.size = rng_.Range(2.5f, 7.0f) * sizeScale_ * 1.2f;
            p.life = 0.0f;
            p.maxLife = rng_.Range(9.0f, 22.0f);
            p.temperature = rng_.Range(300.0f, 420.0f);
            p.opacity = 0.0f;
            p.growth = rng_.Range(1.2f, 3.2f) * sizeScale_;
            p.drag = 0.0f;
            p.seed = rng_.Float01() * 1000.0f;
            p.kind = ParticleKind::Dust;
            p.alive = true;
        }
    }

    (void)burst;
}

void ParticleSystem::Update(float dt, const BlastModel& model, core::ThreadPool& pool) {
    if (dt <= 0.0f) {
        return;
    }

    const BlastState& state = model.State();
    const Environment& env = model.GetEnvironment();

    UpdateSpawners(dt, model);

    Forces forces{};
    forces.wind = XMFLOAT3(float(env.WindX()), 0.0f, float(env.WindZ()));
    forces.ambientTemperature = float(env.temperatureK);
    forces.ambientDensity = float(env.AirDensity());
    forces.gravity = float(env.gravity);
    forces.fireballRadius = float(state.fireballRadius);
    forces.fireballTemperature = float(state.fireballTemperatureK);
    forces.fireballCenter = XMFLOAT3(0.0f, float(state.fireballCenterY), 0.0f);
    forces.cloudRadius = float(state.cloudRadius);
    forces.cloudCenter =
        XMFLOAT3(float(state.cloudDriftX), float(state.cloudCenterY), float(state.cloudDriftZ));
    forces.time = float(state.time);

    ambientTemperature_ = float(env.temperatureK);

    const int count = int(particles_.size());
    pool.ParallelFor(count, 2048, [this, dt, &forces](int begin, int end) {
        IntegrateRange(begin, end, dt, forces);
    });

    BuildBatches();
}

void ParticleSystem::IntegrateRange(int begin, int end, float dt, const Forces& forces) {
    for (int i = begin; i < end; ++i) {
        Particle& p = particles_[i];
        if (!p.alive) {
            continue;
        }

        p.life += dt;
        const float lifeRatio = p.life / std::max(p.maxLife, 1e-3f);
        if (lifeRatio >= 1.0f) {
            p.alive = false;
            continue;
        }

        switch (p.kind) {
        case ParticleKind::Debris: {
            const float speed = Length(p.velocity);
            const float dragMagnitude = p.drag * speed;
            p.velocity.x -= p.velocity.x * dragMagnitude * dt;
            p.velocity.z -= p.velocity.z * dragMagnitude * dt;
            p.velocity.y -= (p.velocity.y * dragMagnitude + forces.gravity) * dt;

            p.position = Add(p.position, Scale(p.velocity, dt));

            if (p.position.y < p.size * 0.5f) {
                p.position.y = p.size * 0.5f;
                if (p.velocity.y < 0.0f) {
                    p.velocity.y = -p.velocity.y * 0.22f;
                    p.velocity.x *= 0.55f;
                    p.velocity.z *= 0.55f;
                }
            }

            // Radiative cooling of the fragment surface.
            p.temperature = forces.ambientTemperature +
                            (p.temperature - forces.ambientTemperature) * std::exp(-dt / 1.6f);
            p.opacity = 1.0f;
            break;
        }
        case ParticleKind::Spark: {
            const float speed = Length(p.velocity);
            const float dragMagnitude = p.drag * speed;
            p.velocity = Add(p.velocity, Scale(p.velocity, -dragMagnitude * dt));
            p.velocity.y -= forces.gravity * dt;
            p.position = Add(p.position, Scale(p.velocity, dt));

            p.temperature = forces.ambientTemperature +
                            (p.temperature - forces.ambientTemperature) * std::exp(-dt / 0.35f);
            // Sparks fade out as they cool.
            p.opacity = std::clamp((p.temperature - 800.0f) / 1200.0f, 0.0f, 1.0f) *
                        (1.0f - lifeRatio * lifeRatio);
            break;
        }
        case ParticleKind::Fire: {
            const float speed = Length(p.velocity);
            const float dragMagnitude = p.drag * speed;
            p.velocity = Add(p.velocity, Scale(p.velocity, -dragMagnitude * dt));
            p.velocity.y += (8.0f - p.velocity.y * 0.4f) * dt;

            const float noiseX =
                HashNoise(p.position.x * 4.0f, p.position.y * 4.0f, forces.time * 8.0f) - 0.5f;
            const float noiseZ =
                HashNoise(p.position.z * 4.0f, p.seed, forces.time * 8.0f) - 0.5f;
            p.velocity.x += noiseX * dt * 30.0f;
            p.velocity.z += noiseZ * dt * 30.0f;

            p.position = Add(p.position, Scale(p.velocity, dt));
            p.size += p.growth * dt;
            p.temperature = forces.ambientTemperature +
                            (p.temperature - forces.ambientTemperature) * std::exp(-dt / 0.11f);
            p.opacity = std::pow(std::max(0.0f, 1.0f - lifeRatio), 1.4f);
            break;
        }
        case ParticleKind::Smoke: {
            const float excess = (p.temperature - forces.ambientTemperature) / forces.ambientTemperature;
            const float buoyancy = forces.gravity * std::clamp(excess, 0.0f, 4.0f) * 0.22f;

            // Entrainment: relax towards the wind field.
            const float relax = 1.0f - std::exp(-dt * 0.35f);
            p.velocity.x += (forces.wind.x - p.velocity.x) * relax;
            p.velocity.z += (forces.wind.z - p.velocity.z) * relax;
            p.velocity.y += (buoyancy - p.velocity.y * 0.05f) * dt;

            // Large-scale turbulent motion.
            const float noiseX = HashNoise(p.position.x * 0.05f, p.position.y * 0.05f, forces.time * 0.4f) - 0.5f;
            const float noiseZ = HashNoise(p.position.z * 0.05f, p.seed, forces.time * 0.4f) - 0.5f;
            p.velocity.x += noiseX * dt * 9.0f;
            p.velocity.z += noiseZ * dt * 9.0f;

            p.position = Add(p.position, Scale(p.velocity, dt));
            p.size += p.growth * dt;
            p.temperature = forces.ambientTemperature +
                            (p.temperature - forces.ambientTemperature) * std::exp(-dt / 2.4f);

            const float fadeIn = std::clamp(p.life / 0.6f, 0.0f, 1.0f);
            const float fadeOut = 1.0f - std::clamp((lifeRatio - 0.55f) / 0.45f, 0.0f, 1.0f);
            p.opacity = 0.65f * fadeIn * fadeOut;
            break;
        }
        case ParticleKind::Dust: {
            const float excess = (p.temperature - forces.ambientTemperature) / forces.ambientTemperature;
            const float buoyancy = forces.gravity * std::clamp(excess, 0.0f, 1.0f) * 0.08f;

            const float relax = 1.0f - std::exp(-dt * 0.8f);
            p.velocity.x += (forces.wind.x - p.velocity.x) * relax;
            p.velocity.z += (forces.wind.z - p.velocity.z) * relax;
            p.velocity.y += (buoyancy - forces.gravity * 0.02f) * dt - p.velocity.y * 0.15f * dt;

            p.position = Add(p.position, Scale(p.velocity, dt));
            if (p.position.y < 0.5f) {
                p.position.y = 0.5f;
                p.velocity.y = std::max(0.0f, p.velocity.y);
            }
            p.size += p.growth * dt;
            p.temperature = forces.ambientTemperature +
                            (p.temperature - forces.ambientTemperature) * std::exp(-dt / 3.0f);

            const float fadeIn = std::clamp(p.life / 1.2f, 0.0f, 1.0f);
            const float fadeOut = 1.0f - std::clamp((lifeRatio - 0.5f) / 0.5f, 0.0f, 1.0f);
            p.opacity = 0.30f * fadeIn * fadeOut;
            break;
        }
        default:
            break;
        }
    }
}

void ParticleSystem::BuildBatches() {
    additive_.clear();
    alpha_.clear();
    fire_.clear();
    aliveCount_ = 0;
    for (int i = 0; i < int(ParticleKind::Count); ++i) {
        aliveByKind_[i] = 0;
    }

    for (const Particle& p : particles_) {
        if (!p.alive) {
            continue;
        }
        ++aliveCount_;
        ++aliveByKind_[int(p.kind)];

        ParticleInstance instance{};
        instance.position = p.position;
        instance.size = p.size;
        instance.opacity = p.opacity;
        instance.rotation = p.seed;
        instance.variation = p.seed * 0.731f;

        if (p.kind == ParticleKind::Debris || p.kind == ParticleKind::Spark) {
            const XMFLOAT3 color = BlackbodyColor(p.temperature);
            instance.color = color;
            instance.pad0 = 1.0f;
            additive_.push_back(instance);
        } else if (p.kind == ParticleKind::Fire) {
            instance.color = BlackbodyColor(p.temperature);
            instance.opacity = p.opacity;
            // Flames stay screen-aligned so the procedural tongue always points
            // upwards; the variation field still randomises the turbulence.
            instance.rotation = 0.0f;
            instance.pad0 = 0.25f;
            fire_.push_back(instance);
        } else {
            // Lit albedo; the particle shader applies a cheap directional term.
            const float warmth = std::clamp((p.temperature - ambientTemperature_) / 900.0f, 0.0f, 1.0f);
            const XMFLOAT3 cool = p.kind == ParticleKind::Smoke ? XMFLOAT3(0.18f, 0.17f, 0.16f)
                                                                : XMFLOAT3(0.55f, 0.46f, 0.34f);
            const XMFLOAT3 warm = XMFLOAT3(0.85f, 0.42f, 0.16f);
            instance.color = XMFLOAT3(cool.x + (warm.x - cool.x) * warmth * 0.25f,
                                      cool.y + (warm.y - cool.y) * warmth * 0.25f,
                                      cool.z + (warm.z - cool.z) * warmth * 0.25f);
            alpha_.push_back(instance);
        }
    }
}

} // namespace ex::sim
