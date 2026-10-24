#pragma once

#include <DirectXMath.h>

#include <vector>

namespace ex::scene {

// 6-DOF rigid body with an isotropic inertia approximation. Boxes are used for
// ground contacts, bounding spheres for body-body contacts; this keeps the
// solver stable and deterministic while still producing tumbling and piling.
struct RigidBody {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 velocity{};
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 angularVelocity{};
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    float boundingRadius = 0.5f;
    float mass = 1.0f;
    float invMass = 1.0f;
    float invInertia = 1.0f;
    float restitution = 0.2f;
    float friction = 0.6f;
    float linearDamping = 0.02f;
    float angularDamping = 0.05f;
    float sleepTimer = 0.0f;
    bool sleeping = false;
};

class PhysicsWorld {
public:
    void Clear();
    int AddBody(const RigidBody& body);
    void Step(float dt);

    const std::vector<RigidBody>& Bodies() const { return bodies_; }
    std::vector<RigidBody>& Bodies() { return bodies_; }

private:
    void ResolveGround(RigidBody& body, float dt);
    void ResolvePair(RigidBody& a, RigidBody& b);

    std::vector<RigidBody> bodies_;
};

} // namespace ex::scene
