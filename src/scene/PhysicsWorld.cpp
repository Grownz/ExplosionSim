#include "scene/PhysicsWorld.h"

#include <atomic>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

namespace ex::scene {
namespace {

// Settle thresholds: a body that stays this calm for long enough while resting
// on static geometry is frozen into a static collider.
constexpr float kSettleSpeed = 0.08f;   // m/s
constexpr float kSettleSpin = 0.5f;     // rad/s
constexpr float kSettleSeconds = 2.0f;
constexpr float kWakeSpeed = 0.5f; // impactor speed that wakes frozen debris

float Length(const DirectX::XMFLOAT3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y +
                     value.z * value.z);
}

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

JPH::RefConst<JPH::Shape> BuildHullShape(
    const std::vector<DirectX::XMFLOAT3>& hullPoints) {
    if (hullPoints.size() < 4) {
        return nullptr;
    }
    JPH::Array<JPH::Vec3> points;
    points.reserve(hullPoints.size());
    for (const DirectX::XMFLOAT3& point : hullPoints) {
        points.push_back(ToJolt(point));
    }
    JPH::ConvexHullShapeSettings hullSettings(points);
    const JPH::ShapeSettings::ShapeResult result = hullSettings.Create();
    if (!result.IsValid()) {
        return nullptr;
    }
    return result.Get();
}

} // namespace

class CollisionShape {
public:
    JPH::RefConst<JPH::Shape> shape;
};

CollisionShapeRef PhysicsWorld::CreateHullShape(
    const std::vector<DirectX::XMFLOAT3>& points) {
    EnsureJoltRuntime();
    JPH::RefConst<JPH::Shape> shape = BuildHullShape(points);
    if (!shape) {
        return nullptr;
    }
    auto result = std::make_shared<CollisionShape>();
    result->shape = std::move(shape);
    return result;
}

static std::atomic<int> g_jobThreads{-1};

struct PhysicsWorld::Impl {
    static constexpr JPH::uint kMaxBodies = 65536;
    static constexpr JPH::uint kMaxBodyPairs = 262144;
    static constexpr JPH::uint kMaxContactConstraints = 65536;

    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl tempAllocator{32 * 1024 * 1024};
    JPH::JobSystemThreadPool jobSystem{JPH::uint(JPH::cMaxPhysicsJobs),
                                       JPH::uint(JPH::cMaxPhysicsBarriers), g_jobThreads.load()};
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
        // A debris pile is one big contact island; a slightly wider sleep window
        // lets a quiet pile settle instead of being kept awake by its slowest
        // member.
        JPH::PhysicsSettings physicsSettings = system.GetPhysicsSettings();
        physicsSettings.mPointVelocitySleepThreshold = 0.05f;
        physicsSettings.mTimeBeforeSleep = 0.3f;
        system.SetPhysicsSettings(physicsSettings);
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

void PhysicsWorld::SetJobThreads(int threads) {
    g_jobThreads.store(threads);
}

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
    watched_.clear();
    activeStamp_.clear();
    stepStamp_ = 0;
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

    // Voronoi chunks use their real convex hull. The fitted box overlaps the
    // neighbouring cells, which made Jolt push the pieces apart every step and
    // kept the whole debris island awake forever. The scene normally supplies
    // the hull prebuilt (CreateHullShape); building it here is the fallback.
    JPH::RefConst<JPH::Shape> shape;
    if (copy.shape) {
        shape = copy.shape->shape;
    } else {
        shape = BuildHullShape(copy.hullPoints);
    }
    if (!shape) {
        shape = new JPH::BoxShape(JPH::Vec3(hx, hy, hz));
    }
    // The Jolt body keeps the shape and its own copy of the points; the
    // mirror does not need them.
    copy.hullPoints.clear();
    copy.hullPoints.shrink_to_fit();
    copy.shape.reset();

    const int index = int(bodies_.size());
    bodies_.push_back(copy);
    if (!copy.isStatic) {
        watched_.push_back(index);
    }
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
    settings.mAllowDynamicOrKinematic = true;
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

    // Mirror the moving bodies. Static structure never moves and a sleeping
    // body keeps its pose, so only Jolt's active list is read, plus the bodies
    // that were awake before (they get one last copy on the step they fall
    // asleep). Walking every body cost a Jolt query per body and step once
    // thousands of chunks existed.
    if (activeStamp_.size() < bodies_.size()) {
        activeStamp_.resize(bodies_.size(), 0);
    }
    if (++stepStamp_ == 0) {
        std::fill(activeStamp_.begin(), activeStamp_.end(), 0u);
        stepStamp_ = 1;
    }
    const JPH::BodyLockInterfaceNoLock& lockInterface =
        impl_->system.GetBodyLockInterfaceNoLock();
    const auto copyState = [&](const JPH::Body& joltBody, RigidBody& body,
                               bool active) {
        body.position = FromJolt(joltBody.GetPosition());
        body.orientation = FromJolt(joltBody.GetRotation());
        body.velocity = FromJolt(joltBody.GetLinearVelocity());
        body.angularVelocity = FromJoltAngular(joltBody.GetAngularVelocity());
        body.sleeping = !active;
    };
    JPH::BodyIDVector active;
    impl_->system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
    std::vector<int> nowActive;
    nowActive.reserve(active.size());
    for (const JPH::BodyID& id : active) {
        const JPH::Body* joltBody = lockInterface.TryGetBody(id);
        if (!joltBody) {
            continue;
        }
        const uint64_t index = joltBody->GetUserData();
        if (index >= bodies_.size()) {
            continue;
        }
        RigidBody& body = bodies_[size_t(index)];
        if (body.isStatic) {
            continue;
        }
        copyState(*joltBody, body, true);
        activeStamp_[size_t(index)] = stepStamp_;
        nowActive.push_back(int(index));
    }
    for (const int index : watched_) {
        if (index < 0 || index >= int(bodies_.size()) ||
            index >= int(impl_->ids.size()) ||
            activeStamp_[size_t(index)] == stepStamp_) {
            continue;
        }
        RigidBody& body = bodies_[size_t(index)];
        if (body.isStatic || body.sleeping) {
            continue;
        }
        const JPH::Body* joltBody = lockInterface.TryGetBody(impl_->ids[index]);
        if (joltBody) {
            copyState(*joltBody, body, false);
        }
    }
    watched_ = std::move(nowActive);

    // Settle: Jolt only sleeps whole islands, and a debris pile is one island,
    // so a few moving chunks kept thousands of resting ones simulated. Bodies
    // that have been quiet for a while and rest on static geometry are frozen
    // into static colliders; an impact wakes them again below.
    for (size_t i = 0; i < bodies_.size(); ++i) {
        RigidBody& body = bodies_[i];
        if (body.isStatic) {
            continue;
        }
        const float speed = Length(body.velocity);
        const float spin = Length(body.angularVelocity);
        if (speed < kSettleSpeed && spin < kSettleSpin) {
            body.sleepTimer += dt;
        } else {
            body.sleepTimer = 0.0f;
        }
        if (body.sleepTimer >= kSettleSeconds && HasStaticSupport(int(i))) {
            FreezeBody(int(i));
        }
    }

    contactEvents_ = impl_->contactCollector.Take();

    // Moving debris that hits a frozen piece wakes it so it can be pushed.
    for (const ContactEvent& event : contactEvents_) {
        int frozenIndex = -1;
        int other = -1;
        if (IsFrozenIndex(event.bodyA)) {
            frozenIndex = event.bodyA;
            other = event.bodyB;
        } else if (IsFrozenIndex(event.bodyB)) {
            frozenIndex = event.bodyB;
            other = event.bodyA;
        }
        if (frozenIndex < 0 || other < 0 || other >= int(bodies_.size())) {
            continue;
        }
        if (Length(bodies_[other].velocity) > kWakeSpeed) {
            WakeBody(frozenIndex);
        }
    }
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

PhysicsWorld::SleepStats PhysicsWorld::CollectSleepStats(float threshold) const {
    SleepStats stats;
    double speedSum = 0.0;
    bool haveY = false;
    for (const RigidBody& body : bodies_) {
        if (body.frozen) {
            ++stats.frozen;
        }
        if (body.isStatic) {
            continue;
        }
        ++stats.dynamic;
        if (!haveY) {
            stats.minY = stats.maxY = body.position.y;
            haveY = true;
        } else {
            stats.minY = std::min(stats.minY, body.position.y);
            stats.maxY = std::max(stats.maxY, body.position.y);
        }
        if (body.sleeping) {
            continue;
        }
        ++stats.awake;
        const float speed = std::sqrt(body.velocity.x * body.velocity.x +
                                      body.velocity.y * body.velocity.y +
                                      body.velocity.z * body.velocity.z);
        speedSum += double(speed);
        if (speed > stats.maxSpeed) {
            stats.maxSpeed = speed;
            stats.fastestPosition = body.position;
        }
        if (speed < threshold) {
            ++stats.belowThreshold;
        }
        if (speed > 1.0f) {
            ++stats.above1m;
        }
        if (speed > 5.0f) {
            ++stats.above5m;
        }
    }
    if (stats.awake > 0) {
        stats.meanSpeed = float(speedSum / double(stats.awake));
    }
    return stats;
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
    watched_.push_back(index);

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

bool PhysicsWorld::IsFrozenIndex(int index) const {
    return index >= 0 && index < int(bodies_.size()) && bodies_[index].frozen;
}

bool PhysicsWorld::HasStaticSupport(int index) const {
    if (!impl_ || index < 0 || index >= int(bodies_.size())) {
        return false;
    }
    const RigidBody& body = bodies_[index];
    // A short downward ray against the non-moving layer (ground, structure and
    // already frozen debris). Casting here means a piece only freezes once it
    // really rests on something that cannot move away underneath it.
    const float reach = std::max(body.boundingRadius, 0.1f) + 0.15f;
    const JPH::RRayCast ray{ToJolt(body.position),
                            JPH::Vec3(0.0f, -reach, 0.0f)};
    JPH::RayCastResult hit;
    const JPH::SpecifiedObjectLayerFilter layerFilter(Layers::NON_MOVING);
    return impl_->system.GetNarrowPhaseQuery().CastRay(ray, hit, {},
                                                       layerFilter);
}

void PhysicsWorld::FreezeBody(int index) {
    if (!impl_ || index < 0 || index >= int(bodies_.size()) ||
        index >= int(impl_->ids.size())) {
        return;
    }
    RigidBody& body = bodies_[index];
    if (body.isStatic) {
        return;
    }
    const JPH::BodyID id = impl_->ids[index];
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    bodyInterface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(),
                                              JPH::Vec3::sZero());
    // Frozen debris joins the non-moving layer so the support ray and the
    // contact filters treat it like ground.
    bodyInterface.SetObjectLayer(id, Layers::NON_MOVING);
    bodyInterface.SetMotionType(id, JPH::EMotionType::Static,
                                JPH::EActivation::DontActivate);
    body.isStatic = true;
    body.frozen = true;
    body.sleeping = false;
    body.sleepTimer = 0.0f;
    body.velocity = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    body.angularVelocity = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
}

void PhysicsWorld::WakeBody(int index) {
    if (!impl_ || index < 0 || index >= int(bodies_.size())) {
        return;
    }
    RigidBody& body = bodies_[index];
    if (!body.frozen) {
        return;
    }
    body.frozen = false;
    body.sleepTimer = 0.0f;
    SetBodyDynamic(index, body.mass, DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f),
                   DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f));
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

void PhysicsWorld::AddAngularImpulse(int index,
                                     const DirectX::XMFLOAT3& impulse) {
    if (!impl_ || index < 0 || index >= int(bodies_.size()) ||
        index >= int(impl_->ids.size())) {
        return;
    }
    const JPH::BodyID id = impl_->ids[index];
    JPH::BodyInterface& bodyInterface = impl_->system.GetBodyInterface();
    if (bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic) {
        return;
    }
    bodyInterface.AddAngularImpulse(id, ToJolt(impulse));
    bodyInterface.ActivateBody(id);
}

} // namespace ex::scene
