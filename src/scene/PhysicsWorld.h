#pragma once

#include <DirectXMath.h>

#include <cstdint>
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
    bool sleeping = false;
};

// Box rigid bodies with a uniform-grid broadphase, oriented-box contacts and
// per-body sleeping.
class PhysicsWorld {
public:
    void Clear();
    int AddBody(const RigidBody& body);
    void Step(float dt);

    const std::vector<RigidBody>& Bodies() const { return bodies_; }
    std::vector<RigidBody>& Bodies() { return bodies_; }
    int BodyCount() const { return int(bodies_.size()); }
    int AwakeCount() const;

private:
    struct CellSlot {
        uint64_t key = 0;
        int head = -1;
    };

    // Contact data kept after the pair solve so the shock-propagation pass can
    // re-solve each contact with the lower body treated as static.
    struct ShockContact {
        int bodyA = -1;
        int bodyB = -1;
        DirectX::XMFLOAT3 normal{0.0f, 1.0f, 0.0f};
        DirectX::XMFLOAT3 points[4]{};
        int count = 0;
        float height = 0.0f;
        float friction = 0.6f;
    };

    void BuildBroadphase();
    void ShockPropagation();
    void ResolveGround(RigidBody& body, float dt);
    void ResolvePair(int indexA, int indexB);

    std::vector<RigidBody> bodies_;

    // Broadphase scratch, reused between steps.
    std::vector<CellSlot> slots_;
    std::vector<int> usedSlots_;
    std::vector<int> insertBody_;
    std::vector<int> insertNext_;
    std::vector<uint64_t> pairs_;
    std::vector<DirectX::XMFLOAT3> aabbMin_;
    std::vector<DirectX::XMFLOAT3> aabbMax_;
    std::vector<ShockContact> shockContacts_;
};

} // namespace ex::scene
