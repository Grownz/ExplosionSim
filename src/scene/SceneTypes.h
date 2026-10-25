#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <utility>
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
    // 0 = free-air blast field, 1 = perfectly reflecting ground.
    float groundReflection = 0.0f;
    // Phase B chunk geometry: Voronoi cells clipped to the building box instead
    // of the jittered grid. This is the default; the grid remains available as
    // the cheaper fallback.
    bool voronoiChunks = true;
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
    // Orientation of the collision box (identity for the grid chunks).
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT4 color{};
    float volume = 0.0f;
    float surfaceArea = 0.0f;
    int bodyIndex = -1;
    int gridY = 0;
    bool detached = false;
    // Convex hull in building space, flat-shaded. Only filled for Voronoi
    // chunks; the grid path keeps using the shared beveled-box mesh.
    std::vector<DirectX::XMFLOAT3> hullPositions;
    std::vector<DirectX::XMFLOAT3> hullNormals;
    std::vector<uint32_t> hullIndices;
    std::vector<int> neighbors;
    std::vector<uint8_t> neighborBroken; // parallel to neighbors
};

struct SceneFireSpawn {
    DirectX::XMFLOAT3 position{};
    int count = 0;
    float scale = 0.5f;
};

// Generation summary, mainly used for logging and benchmarks.
struct SceneStats {
    int buildings = 0;
    int vehicles = 0;
    int trees = 0;
    int chunks = 0;
    int bodies = 0;
    int staticBodies = 0;
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
    bool collapsing = false;
    bool structureRegistered = false;
    int occluders = 0;
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
    SceneStats Stats() const;

    // Damage events accumulated since the last call (drained by Simulation).
    std::vector<SceneDebrisSpawn> ConsumeDebrisSpawns();
    std::vector<SceneFireSpawn> ConsumeFireSpawns();

    const PhysicsWorld& Physics() const { return physics_; }

private:
    struct PendingJointFailure {
        double time = 0.0;
        int objectIndex = -1;
        int chunkA = -1;
        int chunkB = -1;
    };

    void ActivateBody(SceneObject& object);
    void StartCollapse(SceneObject& object);
    void UpdateSupport(SceneObject& object);
    std::vector<uint8_t> ComputeSupport(const SceneObject& object) const;
    void DetachChunk(SceneObject& object, int index);
    void BreakJoint(SceneObject& object, int a, int b);
    void ProcessJointFailures();
    void RegisterStructureBodies(SceneObject& object,
                                 const std::vector<uint8_t>& supported);
    void ApplyContactDamage(float dtSim);
    void StepPhysics(float dtSim);

    SceneSettings settings_{};
    std::vector<SceneObject> objects_;
    std::vector<SceneDebrisSpawn> debrisSpawns_;
    std::vector<SceneFireSpawn> fireSpawns_;
    std::vector<PendingJointFailure> pendingFailures_;
    // Maps a physics body index back to (object, chunk) for contact damage.
    std::vector<std::pair<int, int>> bodyChunk_;
    PhysicsWorld physics_;
    uint64_t hash_ = 0;
    float extent_ = 0.0f;
    double time_ = 0.0;
    double physicsAccumulator_ = 0.0;
};

} // namespace ex::scene
