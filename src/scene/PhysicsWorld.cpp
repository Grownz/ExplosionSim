#include "scene/PhysicsWorld.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

namespace ex::scene {
namespace {

// The scene uses DirectX's left-handed world. Jolt is right-handed, so every
// state crossing the boundary is mirrored on Z. Rotations are axial vectors and
// flip their X/Y components, angular velocities additionally change sign
// (pseudovector under a reflection).
JPH::Vec3 ToJolt(const DirectX::XMFLOAT3& value) {
    return JPH::Vec3(value.x, value.y, -value.z);
}
DirectX::XMFLOAT3 FromJolt(const JPH::Vec3& value) {
    return DirectX::XMFLOAT3(value.GetX(), value.GetY(), -value.GetZ());
}
JPH::Quat ToJolt(const DirectX::XMFLOAT4& value) {
    return JPH::Quat(-value.x, -value.y, value.z, value.w).Normalized();
}
DirectX::XMFLOAT4 FromJolt(const JPH::Quat& value) {
    return DirectX::XMFLOAT4(-value.GetX(), -value.GetY(), value.GetZ(),
                             value.GetW());
}
JPH::Vec3 ToJoltAngular(const DirectX::XMFLOAT3& value) {
    return JPH::Vec3(-value.x, -value.y, value.z);
}
DirectX::XMFLOAT3 FromJoltAngular(const JPH::Vec3& value) {
    return DirectX::XMFLOAT3(-value.GetX(), -value.GetY(), value.GetZ());
}

namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer COUNT = 2;
}

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr JPH::uint COUNT = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        objectToBroadPhase_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        objectToBroadPhase_[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    JPH::uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::COUNT;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer layer) const override {
        return objectToBroadPhase_[layer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        switch (layer.GetValue()) {
        case 0:
            return "NON_MOVING";
        case 1:
            return "MOVING";
        default:
            return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer objectToBroadPhase_[Layers::COUNT];
};

class ObjectVsBroadPhaseLayerFilterImpl final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer,
                       JPH::BroadPhaseLayer broadPhaseLayer) const override {
        switch (layer) {
        case Layers::NON_MOVING:
            return broadPhaseLayer == BroadPhaseLayers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        switch (a) {
        case Layers::NON_MOVING:
            return b == Layers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

// Turns Jolt contact events into the impulse reports the scene consumes for
// structural damage. Jolt does not expose the solver's applied impulse, but
// EstimateCollisionResponse is intended exactly for this use case.
class ContactCollector final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override {
        if (!body1.IsStatic() && !body2.IsStatic()) {
            return;
        }
        JPH::CollisionEstimationResult result;
        JPH::EstimateCollisionResponse(body1, body2, manifold, result,
                                       settings.mCombinedFriction,
                                       settings.mCombinedRestitution);
        float impulse = 0.0f;
        const uint32_t count = std::min<uint32_t>(
            uint32_t(manifold.mRelativeContactPointsOn1.size()),
            uint32_t(result.mContactImpulse.size()));
        for (uint32_t i = 0; i < count; ++i) {
            impulse += std::abs(result.mContactImpulse[i]);
        }
        if (impulse < 20.0f) {
            return;
        }
        ContactEvent event;
        event.bodyA = int(body1.GetUserData());
        event.bodyB = int(body2.GetUserData());
        event.impulse = impulse;
        const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
        event.position = FromJolt(point);
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.size() < 4096) {
            events_.push_back(event);
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.clear();
    }

    std::vector<ContactEvent> Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(events_);
    }

private:
    std::mutex mutex_;
    std::vector<ContactEvent> events_;
};

// Jolt's factory/type registry is process-wide. Register once and tear down at
// exit, independent of how many PhysicsWorld instances the tests create.
void EnsureJoltRuntime() {
    struct Runtime {
        Runtime() {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
        ~Runtime() {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    };
    static Runtime runtime;
    (void)runtime;
}

} // namespace

struct PhysicsWorld::Impl {
    static constexpr JPH::uint kMaxBodies = 65536;
    static constexpr JPH::uint kMaxBodyPairs = 262144;
    static constexpr JPH::uint kMaxContactConstraints = 65536;

    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl tempAllocator{32 * 1024 * 1024};
    JPH::JobSystemThreadPool jobSystem{JPH::uint(JPH::cMaxPhysicsJobs),
                                       JPH::uint(JPH::cMaxPhysicsBarriers), -1};
    BPLayerInterfaceImpl bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBpFilter;
    ObjectLayerPairFilterImpl pairFilter;
    ContactCollector contactCollector;
    std::vector<JPH::BodyID> ids;
    JPH::BodyID groundId;

    Impl() {
        system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContactConstraints,
                    bpLayerInterface, objectVsBpFilter, pairFilter);
        system.SetGravity(JPH::Vec3(0.0f, -9.80665f, 0.0f));
        system.SetContactListener(&contactCollector);

        // Static ground: a large box whose top face lies on y = 0.
        JPH::RefConst<JPH::Shape> groundShape =
            new JPH::BoxShape(JPH::Vec3(5000.0f, 1.0f, 5000.0f));
        JPH::BodyCreationSettings groundSettings(
            groundShape.GetPtr(), JPH::RVec3(0.0f, -1.0f, 0.0f),
            JPH::Quat::sIdentity(), JPH::EMotionType::Static,
            Layers::NON_MOVING);
        groundSettings.mFriction = 0.8f;
        groundSettings.mRestitution = 0.0f;
        groundSettings.mUserData = std::numeric_limits<uint64_t>::max();
        groundId = system.GetBodyInterface().CreateAndAddBody(
            groundSettings, JPH::EActivation::DontActivate);
    }
};

PhysicsWorld::PhysicsWorld() {
    EnsureJoltRuntime();
    impl_ = std::make_unique<Impl>();
}

PhysicsWorld::~PhysicsWorld() {
    if (impl_) {
        Clear();
    }
}

PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

void PhysicsWorld::Clear() {
    if (!impl_) {
        return;
    }
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    for (const JPH::BodyID& id : impl_->ids) {
        if (id.IsInvalid()) {
            continue;
        }
        bodyInterface.RemoveBody(id);
        bodyInterface.DestroyBody(id);
    }
    impl_->ids.clear();
    bodies_.clear();
    contactEvents_.clear();
    impl_->contactCollector.Clear();
}

int PhysicsWorld::AddBody(const RigidBody& body) {
    if (!impl_) {
        return -1;
    }

    RigidBody copy = body;
    copy.mass = std::max(copy.mass, 1e-3f);
    copy.invMass = 1.0f / copy.mass;
    if (copy.boundingRadius <= 0.01f) {
        copy.boundingRadius =
            std::sqrt(copy.halfExtents.x * copy.halfExtents.x +
                      copy.halfExtents.y * copy.halfExtents.y +
                      copy.halfExtents.z * copy.halfExtents.z);
    }
    copy.boundingRadius = std::max(copy.boundingRadius, 0.1f);
    const float hx = std::max(copy.halfExtents.x, 0.01f);
    const float hy = std::max(copy.halfExtents.y, 0.01f);
    const float hz = std::max(copy.halfExtents.z, 0.01f);
    if (copy.isStatic) {
        copy.invMass = 0.0f;
        copy.invInertiaLocal = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        copy.velocity = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        copy.angularVelocity = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        copy.sleeping = false;
    } else {
        // Solid box inertia: Ixx = m/3 * (hy^2 + hz^2), and so on.
        copy.invInertiaLocal =
            DirectX::XMFLOAT3(3.0f / (copy.mass * (hy * hy + hz * hz)),
                              3.0f / (copy.mass * (hx * hx + hz * hz)),
                              3.0f / (copy.mass * (hx * hx + hy * hy)));
    }

    const int index = int(bodies_.size());
    bodies_.push_back(copy);

    JPH::RefConst<JPH::Shape> shape =
        new JPH::BoxShape(JPH::Vec3(hx, hy, hz));
    JPH::BodyCreationSettings settings(
        shape.GetPtr(), ToJolt(copy.position),
        ToJolt(copy.orientation),
        copy.isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
        copy.isStatic ? Layers::NON_MOVING : Layers::MOVING);
    settings.mFriction = copy.friction;
    settings.mRestitution = copy.restitution;
    settings.mLinearDamping = copy.linearDamping;
    settings.mAngularDamping = copy.angularDamping;
    settings.mAllowSleeping = !copy.isStatic;
    // Standing structure bodies are created static and switched to dynamic when
    // their joints fail; Jolt requires motion properties for that transition.
    settings.mAllowDynamicOrKinematic = copy.isStatic;
    settings.mUserData = uint64_t(index);
    if (!copy.isStatic) {
        settings.mOverrideMassProperties =
            JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = copy.mass;
    }

    const JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(
        settings, copy.isStatic ? JPH::EActivation::DontActivate
                                : JPH::EActivation::Activate);
    impl_->ids.push_back(id);
    return index;
}

void PhysicsWorld::Step(float dt) {
    if (!impl_ || bodies_.empty() || dt <= 0.0f) {
        return;
    }
    impl_->contactCollector.Clear();
    impl_->system.Update(dt, 1, &impl_->tempAllocator, &impl_->jobSystem);

    const JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    for (size_t i = 0; i < impl_->ids.size() && i < bodies_.size(); ++i) {
        const JPH::BodyID id = impl_->ids[i];
        if (id.IsInvalid()) {
            continue;
        }
        JPH::RVec3 position;
        JPH::Quat orientation;
        bodyInterface.GetPositionAndRotation(id, position, orientation);
        JPH::Vec3 linearVelocity;
        JPH::Vec3 angularVelocity;
        bodyInterface.GetLinearAndAngularVelocity(id, linearVelocity,
                                                  angularVelocity);
        RigidBody& body = bodies_[i];
        body.position = FromJolt(position);
        body.orientation = FromJolt(orientation);
        body.velocity = FromJolt(linearVelocity);
        body.angularVelocity = FromJoltAngular(angularVelocity);
        body.isStatic = bodyInterface.GetMotionType(id) == JPH::EMotionType::Static;
        body.sleeping = body.isStatic ? false : !bodyInterface.IsActive(id);
    }
    contactEvents_ = impl_->contactCollector.Take();
}

int PhysicsWorld::AwakeCount() const {
    int count = 0;
    for (const RigidBody& body : bodies_) {
        if (!body.isStatic && !body.sleeping) {
            ++count;
        }
    }
    return count;
}

void PhysicsWorld::SetBodyDynamic(int index, float mass,
                                  const DirectX::XMFLOAT3& velocity,
                                  const DirectX::XMFLOAT3& angularVelocity) {
    if (!impl_ || index < 0 || index >= int(bodies_.size()) ||
        index >= int(impl_->ids.size())) {
        return;
    }

    RigidBody& body = bodies_[index];
    body.isStatic = false;
    body.mass = std::max(mass, 1e-3f);
    body.invMass = 1.0f / body.mass;
    const float hx = std::max(body.halfExtents.x, 0.01f);
    const float hy = std::max(body.halfExtents.y, 0.01f);
    const float hz = std::max(body.halfExtents.z, 0.01f);
    body.invInertiaLocal =
        DirectX::XMFLOAT3(3.0f / (body.mass * (hy * hy + hz * hz)),
                          3.0f / (body.mass * (hx * hx + hz * hz)),
                          3.0f / (body.mass * (hx * hx + hy * hy)));
    body.velocity = velocity;
    body.angularVelocity = angularVelocity;
    body.sleeping = false;
    body.sleepTimer = 0.0f;

    const JPH::BodyID id = impl_->ids[index];
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    bodyInterface.SetObjectLayer(id, Layers::MOVING);
    bodyInterface.SetMotionType(id, JPH::EMotionType::Dynamic,
                                JPH::EActivation::Activate);
    {
        JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), id);
        if (lock.Succeeded()) {
            JPH::Body& joltBody = lock.GetBody();
            JPH::MassProperties properties =
                joltBody.GetShape()->GetMassProperties();
            properties.ScaleToMass(body.mass);
            joltBody.GetMotionProperties()->SetMassProperties(
                JPH::EAllowedDOFs::All, properties);
        }
    }
    bodyInterface.SetLinearAndAngularVelocity(id, ToJolt(velocity),
                                              ToJoltAngular(angularVelocity));
    bodyInterface.ActivateBody(id);
}

void PhysicsWorld::AddImpulse(int index, const DirectX::XMFLOAT3& impulse) {
    if (!impl_ || index < 0 || index >= int(bodies_.size()) ||
        index >= int(impl_->ids.size())) {
        return;
    }
    const JPH::BodyID id = impl_->ids[index];
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    if (bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic) {
        return;
    }
    bodyInterface.AddImpulse(id, ToJolt(impulse));
    bodyInterface.ActivateBody(id);
}

} // namespace ex::scene
