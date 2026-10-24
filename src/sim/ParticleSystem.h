#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

#include "core/Random.h"
#include "sim/BlastModel.h"

namespace ex::core {
class ThreadPool;
}

namespace ex::sim {

enum class ParticleKind : uint8_t {
    Debris = 0,
    Spark = 1,
    Fire = 2,
    Smoke = 3,
    Dust = 4,
    Count = 5,
};

// GPU-facing instance data (48 bytes, std430-friendly).
struct ParticleInstance {
    DirectX::XMFLOAT3 position;
    float size;
    DirectX::XMFLOAT3 color;
    float opacity;
    float rotation;
    float variation;
    float pad0;
    float pad1;
};

// Multithreaded CPU particle system: ballistic debris, glowing sparks, rising
// smoke and shock-driven ground dust.
class ParticleSystem {
public:
    void Initialize(int capacity);
    void Reset();
    void Detonate(const BlastModel& model);
    void Update(float dt, const BlastModel& model, core::ThreadPool& pool);

    // Spawns a generic debris burst for a damaged scene object (main thread).
    void SpawnSceneDebris(const DirectX::XMFLOAT3& center, int count, float sizeScale,
                          float speed);
    // Spawns a small fire burst for a burning scene object (main thread).
    void SpawnSceneFire(const DirectX::XMFLOAT3& center, int count, float scale);

    const std::vector<ParticleInstance>& AdditiveBatch() const { return additive_; }
    const std::vector<ParticleInstance>& AlphaBatch() const { return alpha_; }
    const std::vector<ParticleInstance>& FireBatch() const { return fire_; }

    int AliveCount() const { return aliveCount_; }
    int AliveCount(ParticleKind kind) const { return aliveByKind_[int(kind)]; }
    int Capacity() const { return int(particles_.size()); }

private:
    struct Particle {
        DirectX::XMFLOAT3 position{};
        float size = 0.0f;
        DirectX::XMFLOAT3 velocity{};
        float life = 0.0f;
        float maxLife = 1.0f;
        float temperature = 300.0f;
        float opacity = 1.0f;
        float drag = 0.0f;      // 0.5 * rho * Cd * A / m
        float growth = 0.0f;
        float seed = 0.0f;
        ParticleKind kind = ParticleKind::Debris;
        bool alive = false;
    };

    struct Forces {
        DirectX::XMFLOAT3 wind;
        float ambientTemperature = 288.15f;
        float ambientDensity = 1.225f;
        float gravity = 9.81f;
        float fireballRadius = 0.0f;
        float fireballTemperature = 0.0f;
        DirectX::XMFLOAT3 fireballCenter;
        float cloudRadius = 0.0f;
        DirectX::XMFLOAT3 cloudCenter;
        float time = 0.0f;
    };

    void UpdateSpawners(float dt, const BlastModel& model);
    void IntegrateRange(int begin, int end, float dt, const Forces& forces);
    void BuildBatches();
    void SpawnDebris(const BlastModel& model, int count);
    void SpawnSparks(const BlastModel& model, int count);
    void SpawnBaseDust(const BlastModel& model, int count);
    void SpawnFire(const BlastModel& model, int count);

    int AcquireSlot(ParticleKind kind);

    std::vector<Particle> particles_;
    std::vector<ParticleInstance> additive_;
    std::vector<ParticleInstance> alpha_;
    std::vector<ParticleInstance> fire_;
    core::Rng rng_{0xC0FFEEull};

    int cursor_[int(ParticleKind::Count)]{};
    int rangeBegin_[int(ParticleKind::Count)]{};
    int rangeEnd_[int(ParticleKind::Count)]{};
    int aliveByKind_[int(ParticleKind::Count)]{};
    int aliveCount_ = 0;

    float smokeSpawnAccumulator_ = 0.0f;
    float dustSpawnAccumulator_ = 0.0f;
    float detonationTime_ = 0.0f;
    float ambientTemperature_ = 288.15f;
    float sizeScale_ = 1.0f;
    float countScale_ = 1.0f;
};

} // namespace ex::sim
