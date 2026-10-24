#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

#include "scene/PhysicsWorld.h"
#include "sim/BlastModel.h"

namespace ex::scene {

enum class SceneObjectType : uint8_t {
    Building = 0,
    Vehicle = 1,
    Tree = 2,
    Count = 3,
};

enum class DamageState : uint8_t {
    Intact = 0,
    Light = 1,
    Heavy = 2,
    Destroyed = 3,
};

struct SceneSettings {
    bool enabled = true;
    uint32_t seed = 1337;
    float density = 1.0f;
    bool buildings = true;
    bool vehicles = true;
    bool vegetation = true;
    int quality = 1; // 0 = low, 1 = medium, 2 = high
};

struct SceneDebrisSpawn {
    DirectX::XMFLOAT3 position{};
    int count = 0;
    float scale = 0.4f;
    float speed = 10.0f;
};

struct SceneChunk {
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 half{};
    DirectX::XMFLOAT4 color{};
    int bodyIndex = -1;
};

struct SceneFireSpawn {
    DirectX::XMFLOAT3 position{};
    int count = 0;
    float scale = 0.5f;
};

// One procedurally placed object. The blast fields are evaluated once at
// generation time from the configured analytic model.
struct SceneObject {
    uint32_t id = 0;
    SceneObjectType type = SceneObjectType::Building;
    DirectX::XMFLOAT3 position{}; // base centre on the ground
    float yaw = 0.0f;             // degrees
    DirectX::XMFLOAT3 size{1.0f, 1.0f, 1.0f};
    float distanceToBurst = 0.0f;
    float arrivalTime = 0.0f;
    float peakOverpressurePa = 0.0f;
    float impulsePaS = 0.0f;
    float thermalFluenceJm2 = 0.0f;
    DamageState damage = DamageState::Intact;
    int meshVariant = 0;
    float swayPhase = 0.0f;
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};

    // Runtime state (not part of the generation hash).
    bool resolved = false;
    bool burning = false;
    float damageAmount = 0.0f;
    float burnTimer = 0.0f;
    int bodyIndex = -1;
    bool collapsed = false;
    std::vector<SceneChunk> chunks;
};

// Deterministic procedural scene around ground zero. Generation is
// single-threaded and seeded; the hash allows tests to verify reproducibility.
class Scene {
public:
    void Generate(const SceneSettings& settings, const sim::BlastModel& blast);
    void Clear();
    void Update(const sim::BlastModel& blast, float dtSim);

    const std::vector<SceneObject>& Objects() const { return objects_; }
    const SceneSettings& Settings() const { return settings_; }
    uint64_t GenerationHash() const { return hash_; }
    float Extent() const { return extent_; }
    bool Empty() const { return objects_.empty(); }

    // Damage events accumulated since the last call (drained by Simulation).
    std::vector<SceneDebrisSpawn> ConsumeDebrisSpawns();
    std::vector<SceneFireSpawn> ConsumeFireSpawns();

    const PhysicsWorld& Physics() const { return physics_; }

private:
    void ActivateBody(SceneObject& object);
    void FractureBuilding(SceneObject& object);
    void StepPhysics(float dtSim);

    SceneSettings settings_{};
    std::vector<SceneObject> objects_;
    std::vector<SceneDebrisSpawn> debrisSpawns_;
    std::vector<SceneFireSpawn> fireSpawns_;
    PhysicsWorld physics_;
    uint64_t hash_ = 0;
    float extent_ = 0.0f;
    double time_ = 0.0;
    double physicsAccumulator_ = 0.0;
};

} // namespace ex::scene
