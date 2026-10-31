#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ex::scene {

// 6-DOF rigid body. Collision uses the convex hull when one is supplied (the
// Voronoi chunks), otherwise an oriented box; the inertia tensor is the
// analytic box tensor, stored as a body-space diagonal.
struct RigidBody {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 velocity{};
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 angularVelocity{};
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT3 invInertiaLocal{1.0f, 1.0f, 1.0f};
    float boundingRadius = 0.5f;
    float mass = 1.0f;
    float invMass = 1.0f;
    float restitution = 0.2f;
    float friction = 0.6f;
    float linearDamping = 0.02f;
    float angularDamping = 0.05f;
    float sleepTimer = 0.0f;
    bool isStatic = false;
    bool sleeping = false;
    // Body-local collision hull (only used while the body is created); empty
    // means the box described by halfExtents is used.
    std::vector<DirectX::XMFLOAT3> hullPoints;
};

// A contact impulse worth reporting to the scene (debris hitting structure).
struct ContactEvent {
    int bodyA = -1;
    int bodyB = -1;
    float impulse = 0.0f;
    DirectX::XMFLOAT3 position{};
};

// Box rigid bodies backed by Jolt Physics. The public interface is the same as
// the previous in-house solver: the scene owns body indices, switches standing
// structure bodies to dynamic on detachment and consumes contact events.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    void Clear();
    int AddBody(const RigidBody& body);
    void Step(float dt);

    const std::vector<RigidBody>& Bodies() const { return bodies_; }
    std::vector<RigidBody>& Bodies() { return bodies_; }
    int BodyCount() const { return int(bodies_.size()); }

    // Diagnostic: how many dynamic bodies are still awake and how fast they
    // actually move. Sleeping is island based in Jolt, so a pile of debris can
    // stay awake even when most of its bodies are nearly at rest.
    struct SleepStats {
        int dynamic = 0;
        int awake = 0;
        int belowThreshold = 0; // awake but slower than the sleep threshold
        int above1m = 0;        // awake and faster than 1 m/s
        int above5m = 0;        // awake and faster than 5 m/s
        float meanSpeed = 0.0f;
        float maxSpeed = 0.0f;
        DirectX::XMFLOAT3 fastestPosition{};
        float minY = 0.0f;
        float maxY = 0.0f;
    };
    SleepStats CollectSleepStats(float threshold = 0.03f) const;
    int AwakeCount() const;

    // Converts a static structure body into a dynamic one (chunk detachment).
    void SetBodyDynamic(int index, float mass, const DirectX::XMFLOAT3& velocity,
                        const DirectX::XMFLOAT3& angularVelocity);
    // Applies an impulse to a dynamic body (blast wave loading).
    void AddImpulse(int index, const DirectX::XMFLOAT3& impulse);
    const std::vector<ContactEvent>& ContactEvents() const { return contactEvents_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Mirror of the Jolt body state, used by the renderer and the tests.
    std::vector<RigidBody> bodies_;
    std::vector<ContactEvent> contactEvents_;
};

} // namespace ex::scene
