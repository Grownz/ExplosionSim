#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ex::scene {

// 6-DOF rigid body. Collision uses oriented boxes; the inertia tensor is the
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
    int AwakeCount() const;

    // Converts a static structure body into a dynamic one (chunk detachment).
    void SetBodyDynamic(int index, float mass, const DirectX::XMFLOAT3& velocity,
                        const DirectX::XMFLOAT3& angularVelocity);
    const std::vector<ContactEvent>& ContactEvents() const { return contactEvents_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // Mirror of the Jolt body state, used by the renderer and the tests.
    std::vector<RigidBody> bodies_;
    std::vector<ContactEvent> contactEvents_;
};

} // namespace ex::scene
