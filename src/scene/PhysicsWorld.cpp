#include "scene/PhysicsWorld.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>

using namespace DirectX;

namespace ex::scene {

namespace {

constexpr float kGravity = 9.80665f;
constexpr float kShockSpeed = 0.5f;
constexpr bool kShockPropagation = true;
constexpr float kContactEventImpulse = 500.0f;
constexpr float kCellSize = 4.0f;
constexpr int kGridSlots = 65536;

uint64_t CellKey(int x, int y, int z) {
    const uint64_t ux = uint64_t(uint32_t(x + (1 << 20))) & 0x1FFFFF;
    const uint64_t uy = uint64_t(uint32_t(y + (1 << 20))) & 0x1FFFFF;
    const uint64_t uz = uint64_t(uint32_t(z + (1 << 20))) & 0x1FFFFF;
    return (ux << 42) | (uy << 21) | uz;
}

XMMATRIX InverseInertiaWorld(const RigidBody& body) {
    const XMMATRIX rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation));
    const XMMATRIX diagonal = XMMatrixSet(
        body.invInertiaLocal.x, 0.0f, 0.0f, 0.0f,
        0.0f, body.invInertiaLocal.y, 0.0f, 0.0f,
        0.0f, 0.0f, body.invInertiaLocal.z, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f);
    // Row-vector convention: I_world = R^T * I_local * R.
    return XMMatrixTranspose(rotation) * diagonal * rotation;
}

float HalfAt(const XMFLOAT3& half, int axis) {
    return axis == 0 ? half.x : (axis == 1 ? half.y : half.z);
}

float VectorLength(const XMFLOAT3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

struct Obb {
    XMVECTOR center;
    XMVECTOR axes[3];
    XMFLOAT3 half;
    XMMATRIX rotation;
};

Obb MakeObb(const RigidBody& body) {
    Obb obb;
    obb.center = XMLoadFloat3(&body.position);
    obb.rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation));
    obb.axes[0] = obb.rotation.r[0];
    obb.axes[1] = obb.rotation.r[1];
    obb.axes[2] = obb.rotation.r[2];
    obb.half = body.halfExtents;
    return obb;
}

struct Manifold {
    XMVECTOR normal = XMVectorZero();
    XMVECTOR points[4];
    int count = 0;
    float penetration = 0.0f;
};

bool InsideObb(const XMVECTOR& point, const Obb& obb) {
    const XMVECTOR local = point - obb.center;
    for (int i = 0; i < 3; ++i) {
        const float distance = XMVectorGetX(XMVector3Dot(local, obb.axes[i]));
        if (std::abs(distance) > HalfAt(obb.half, i) + 0.01f) {
            return false;
        }
    }
    return true;
}

void AddPoint(Manifold& manifold, const XMVECTOR& point) {
    if (manifold.count >= 4) {
        return;
    }
    for (int i = 0; i < manifold.count; ++i) {
        if (XMVectorGetX(XMVector3LengthSq(point - manifold.points[i])) < 1e-4f) {
            return;
        }
    }
    manifold.points[manifold.count++] = point;
}

// Separating-axis test. Returns false if the boxes are disjoint.
bool TestAxis(const XMVECTOR& axis, const Obb& a, const Obb& b, float& minOverlap,
              XMVECTOR& bestAxis) {
    const float lengthSquared = XMVectorGetX(XMVector3LengthSq(axis));
    if (lengthSquared < 1e-8f) {
        return true;
    }
    const XMVECTOR normal = XMVectorScale(axis, 1.0f / std::sqrt(lengthSquared));

    float radiusA = 0.0f;
    float radiusB = 0.0f;
    for (int i = 0; i < 3; ++i) {
        radiusA += HalfAt(a.half, i) *
                   std::abs(XMVectorGetX(XMVector3Dot(normal, a.axes[i])));
        radiusB += HalfAt(b.half, i) *
                   std::abs(XMVectorGetX(XMVector3Dot(normal, b.axes[i])));
    }

    const float distance =
        std::abs(XMVectorGetX(XMVector3Dot(b.center - a.center, normal)));
    const float overlap = radiusA + radiusB - distance;
    if (overlap < 0.0f) {
        return false;
    }
    if (overlap < minOverlap) {
        minOverlap = overlap;
        bestAxis = normal;
    }
    return true;
}

void BuildManifold(const Obb& a, const Obb& b, const XMVECTOR& normal, float penetration,
                   Manifold& manifold) {
    manifold.normal = normal;
    manifold.penetration = penetration;
    manifold.count = 0;

    // Vertices of B inside A and vice versa. For face contacts this produces the
    // four incident-face corners; for edge contacts it falls back below.
    for (int i = 0; i < 8; ++i) {
        const XMVECTOR local = XMVectorSet(((i & 1) ? 1.0f : -1.0f) * b.half.x,
                                           ((i & 2) ? 1.0f : -1.0f) * b.half.y,
                                           ((i & 4) ? 1.0f : -1.0f) * b.half.z, 1.0f);
        const XMVECTOR world = XMVector3TransformCoord(local, b.rotation) + b.center;
        if (InsideObb(world, a)) {
            AddPoint(manifold, world);
        }
    }
    for (int i = 0; i < 8; ++i) {
        const XMVECTOR local = XMVectorSet(((i & 1) ? 1.0f : -1.0f) * a.half.x,
                                           ((i & 2) ? 1.0f : -1.0f) * a.half.y,
                                           ((i & 4) ? 1.0f : -1.0f) * a.half.z, 1.0f);
        const XMVECTOR world = XMVector3TransformCoord(local, a.rotation) + a.center;
        if (InsideObb(world, b)) {
            AddPoint(manifold, world);
        }
    }

    if (manifold.count == 0) {
        // Edge-edge fallback: midpoint of the two support planes.
        float supportA = 0.0f;
        float supportB = 0.0f;
        for (int i = 0; i < 3; ++i) {
            supportA += HalfAt(a.half, i) *
                        std::abs(XMVectorGetX(XMVector3Dot(normal, a.axes[i])));
            supportB += HalfAt(b.half, i) *
                        std::abs(XMVectorGetX(XMVector3Dot(normal, b.axes[i])));
        }
        const XMVECTOR pointA = a.center + normal * supportA;
        const XMVECTOR pointB = b.center - normal * supportB;
        AddPoint(manifold, (pointA + pointB) * 0.5f);
    }
}

} // namespace

void PhysicsWorld::Clear() {
    bodies_.clear();
    slots_.clear();
    usedSlots_.clear();
    insertBody_.clear();
    insertNext_.clear();
    pairs_.clear();
    aabbMin_.clear();
    aabbMax_.clear();
    shockContacts_.clear();
}

int PhysicsWorld::AddBody(const RigidBody& body) {
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
        copy.invInertiaLocal = XMFLOAT3(0.0f, 0.0f, 0.0f);
        copy.velocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
        copy.angularVelocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
        copy.sleeping = false;
    } else {
        // Solid box inertia: Ixx = m/3 * (hy^2 + hz^2), and so on.
        copy.invInertiaLocal =
            XMFLOAT3(3.0f / (copy.mass * (hy * hy + hz * hz)),
                     3.0f / (copy.mass * (hx * hx + hz * hz)),
                     3.0f / (copy.mass * (hx * hx + hy * hy)));
    }
    bodies_.push_back(copy);
    return int(bodies_.size()) - 1;
}

void PhysicsWorld::SetBodyDynamic(int index, float mass, const XMFLOAT3& velocity,
                                  const XMFLOAT3& angularVelocity) {
    if (index < 0 || index >= int(bodies_.size())) {
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
        XMFLOAT3(3.0f / (body.mass * (hy * hy + hz * hz)),
                 3.0f / (body.mass * (hx * hx + hz * hz)),
                 3.0f / (body.mass * (hx * hx + hy * hy)));
    body.velocity = velocity;
    body.angularVelocity = angularVelocity;
    body.sleeping = false;
    body.sleepTimer = 0.0f;
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

void PhysicsWorld::Step(float dt) {
    if (dt <= 0.0f) {
        return;
    }

    const XMVECTOR gravity = XMVectorSet(0.0f, -kGravity, 0.0f, 0.0f);

    for (RigidBody& body : bodies_) {
        if (body.isStatic || body.sleeping) {
            continue;
        }

        XMVECTOR velocity = XMLoadFloat3(&body.velocity);
        velocity = XMVectorAdd(velocity, XMVectorScale(gravity, dt));
        velocity = XMVectorScale(velocity, 1.0f / (1.0f + body.linearDamping * dt));
        XMStoreFloat3(&body.velocity, velocity);

        XMVECTOR position = XMLoadFloat3(&body.position);
        position = XMVectorAdd(position, XMVectorScale(velocity, dt));
        XMStoreFloat3(&body.position, position);

        XMVECTOR omega = XMLoadFloat3(&body.angularVelocity);
        omega = XMVectorScale(omega, 1.0f / (1.0f + body.angularDamping * dt));
        XMStoreFloat3(&body.angularVelocity, omega);

        XMVECTOR orientation = XMLoadFloat4(&body.orientation);
        const XMVECTOR omegaQuat =
            XMVectorSet(XMVectorGetX(omega), XMVectorGetY(omega), XMVectorGetZ(omega), 0.0f);
        const XMVECTOR derivative = XMQuaternionMultiply(omegaQuat, orientation);
        orientation = XMVectorAdd(orientation, XMVectorScale(derivative, 0.5f * dt));
        orientation = XMQuaternionNormalize(orientation);
        XMStoreFloat4(&body.orientation, orientation);

        ResolveGround(body, dt);
    }

    BuildBroadphase();
    shockContacts_.clear();
    contactEvents_.clear();
    for (uint64_t key : pairs_) {
        const int a = int(key >> 32);
        const int b = int(key & 0xFFFFFFFFu);
        ResolvePair(a, b);
    }
    if (kShockPropagation) ShockPropagation();

    for (RigidBody& body : bodies_) {
        if (body.isStatic || body.sleeping) {
            continue;
        }
        const float speed = VectorLength(body.velocity);
        const float spin = VectorLength(body.angularVelocity);
        const bool grounded = body.position.y <= body.boundingRadius + 0.6f;
        if (grounded && speed < 0.15f && spin < 0.25f) {
            body.sleepTimer += dt;
            if (body.sleepTimer > 0.7f) {
                body.sleeping = true;
                body.velocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
                body.angularVelocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
            }
        } else {
            body.sleepTimer = 0.0f;
        }
    }
}

void PhysicsWorld::BuildBroadphase() {
    const int count = int(bodies_.size());
    if (int(slots_.size()) != kGridSlots) {
        slots_.assign(kGridSlots, CellSlot{});
    } else {
        for (int slot : usedSlots_) {
            slots_[slot].head = -1;
        }
    }
    usedSlots_.clear();
    insertBody_.clear();
    insertNext_.clear();
    pairs_.clear();
    aabbMin_.resize(count);
    aabbMax_.resize(count);

    auto insert = [&](uint64_t key, int body) {
        uint64_t hash = key * 0x9E3779B97F4A7C15ull;
        hash ^= hash >> 29;
        int slot = int(hash & uint64_t(kGridSlots - 1));
        int probes = 0;
        while (slots_[slot].head >= 0 && slots_[slot].key != key) {
            slot = (slot + 1) & (kGridSlots - 1);
            if (++probes >= kGridSlots) {
                return;
            }
        }
        if (slots_[slot].head < 0) {
            slots_[slot].key = key;
            usedSlots_.push_back(slot);
        }
        insertBody_.push_back(body);
        insertNext_.push_back(slots_[slot].head);
        slots_[slot].head = int(insertBody_.size()) - 1;
    };

    for (int i = 0; i < count; ++i) {
        const RigidBody& body = bodies_[i];
        const XMMATRIX rotation =
            XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation));
        const XMFLOAT3& half = body.halfExtents;
        const float ex = std::abs(XMVectorGetX(rotation.r[0])) * half.x +
                         std::abs(XMVectorGetX(rotation.r[1])) * half.y +
                         std::abs(XMVectorGetX(rotation.r[2])) * half.z;
        const float ey = std::abs(XMVectorGetY(rotation.r[0])) * half.x +
                         std::abs(XMVectorGetY(rotation.r[1])) * half.y +
                         std::abs(XMVectorGetY(rotation.r[2])) * half.z;
        const float ez = std::abs(XMVectorGetZ(rotation.r[0])) * half.x +
                         std::abs(XMVectorGetZ(rotation.r[1])) * half.y +
                         std::abs(XMVectorGetZ(rotation.r[2])) * half.z;

        aabbMin_[i] = XMFLOAT3(body.position.x - ex, body.position.y - ey,
                               body.position.z - ez);
        aabbMax_[i] = XMFLOAT3(body.position.x + ex, body.position.y + ey,
                               body.position.z + ez);

        const int cx0 = int(std::floor(aabbMin_[i].x / kCellSize));
        const int cy0 = int(std::floor(aabbMin_[i].y / kCellSize));
        const int cz0 = int(std::floor(aabbMin_[i].z / kCellSize));
        const int cx1 = int(std::floor(aabbMax_[i].x / kCellSize));
        const int cy1 = int(std::floor(aabbMax_[i].y / kCellSize));
        const int cz1 = int(std::floor(aabbMax_[i].z / kCellSize));
        for (int cx = cx0; cx <= cx1; ++cx) {
            for (int cy = cy0; cy <= cy1; ++cy) {
                for (int cz = cz0; cz <= cz1; ++cz) {
                    insert(CellKey(cx, cy, cz), i);
                }
            }
        }
    }

    for (int slot : usedSlots_) {
        for (int i = slots_[slot].head; i >= 0; i = insertNext_[i]) {
            const int a = insertBody_[i];
            for (int j = insertNext_[i]; j >= 0; j = insertNext_[j]) {
                const int b = insertBody_[j];
                if (a == b) {
                    continue;
                }
                const int lo = a < b ? a : b;
                const int hi = a < b ? b : a;
                if ((bodies_[lo].sleeping && bodies_[hi].sleeping) ||
                    (bodies_[lo].isStatic && bodies_[hi].isStatic)) {
                    continue;
                }
                if (aabbMin_[lo].x > aabbMax_[hi].x || aabbMax_[lo].x < aabbMin_[hi].x ||
                    aabbMin_[lo].y > aabbMax_[hi].y || aabbMax_[lo].y < aabbMin_[hi].y ||
                    aabbMin_[lo].z > aabbMax_[hi].z || aabbMax_[lo].z < aabbMin_[hi].z) {
                    continue;
                }
                pairs_.push_back((uint64_t(uint32_t(lo)) << 32) | uint64_t(uint32_t(hi)));
            }
        }
    }

    std::sort(pairs_.begin(), pairs_.end());
    pairs_.erase(std::unique(pairs_.begin(), pairs_.end()), pairs_.end());
}

void PhysicsWorld::ShockPropagation() {
    // Second pass over the contacts from the ground up. Each upper body is
    // resolved against the lower body as if it were infinitely massive, which
    // propagates the support impulse to the top of a stack in one sweep.
    std::sort(shockContacts_.begin(), shockContacts_.end(),
              [](const ShockContact& a, const ShockContact& b) {
                  return a.height < b.height;
              });

    for (int pass = 0; pass < 3; ++pass) {
        for (const ShockContact& contact : shockContacts_) {
            RigidBody& lower = bodies_[contact.bodyA];
            RigidBody& upper = bodies_[contact.bodyB];
            if (upper.isStatic) {
                continue;
            }
            if (lower.sleeping && upper.sleeping) {
                continue;
            }
            const XMVECTOR normal = XMLoadFloat3(&contact.normal);
            const float lowerNormalVelocity =
                XMVectorGetX(XMVector3Dot(XMLoadFloat3(&lower.velocity), normal));
            const float upperNormalVelocity =
                XMVectorGetX(XMVector3Dot(XMLoadFloat3(&upper.velocity), normal));
            // Resting contacts only: real impacts stay with the normal solver.
            if (lowerNormalVelocity < -kShockSpeed ||
                upperNormalVelocity < -kShockSpeed) {
                continue;
            }

            const XMMATRIX invInertia = InverseInertiaWorld(upper);
            XMVECTOR velocity = XMLoadFloat3(&upper.velocity);
            XMVECTOR omega = XMLoadFloat3(&upper.angularVelocity);
            const XMVECTOR position = XMLoadFloat3(&upper.position);

            for (int i = 0; i < contact.count; ++i) {
                const XMVECTOR point = XMLoadFloat3(&contact.points[i]);
                const XMVECTOR r = point - position;
                const XMVECTOR pointVelocity = velocity + XMVector3Cross(omega, r);
                const float normalVelocity =
                    XMVectorGetX(XMVector3Dot(pointVelocity, normal));
                if (normalVelocity >= 0.0f) {
                    continue;
                }
                const XMVECTOR rn = XMVector3Cross(r, normal);
                const float denominator =
                    upper.invMass +
                    XMVectorGetX(XMVector3Dot(
                        rn, XMVector3TransformNormal(rn, invInertia)));
                if (denominator <= 1e-8f) {
                    continue;
                }
                const float impulse = -normalVelocity / denominator;
                velocity += normal * (impulse * upper.invMass);
                omega += XMVector3TransformNormal(rn * impulse, invInertia);
            }

            XMStoreFloat3(&upper.velocity, velocity);
            XMStoreFloat3(&upper.angularVelocity, omega);
            upper.sleepTimer = 0.0f;
        }
    }

}

void PhysicsWorld::ResolveGround(RigidBody& body, float dt) {
    (void)dt;
    if (body.isStatic) {
        return;
    }

    const XMMATRIX rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation));
    XMVECTOR center = XMLoadFloat3(&body.position);
    XMVECTOR velocity = XMLoadFloat3(&body.velocity);
    XMVECTOR omega = XMLoadFloat3(&body.angularVelocity);

    const XMFLOAT3& half = body.halfExtents;
    XMVECTOR corners[8];
    int index = 0;
    for (float sx : {-1.0f, 1.0f}) {
        for (float sy : {-1.0f, 1.0f}) {
            for (float sz : {-1.0f, 1.0f}) {
                corners[index++] = XMVectorSet(sx * half.x, sy * half.y, sz * half.z, 1.0f);
            }
        }
    }

    float maxPenetration = 0.0f;
    XMVECTOR contactSum = XMVectorZero();
    int contactCount = 0;
    for (const XMVECTOR& local : corners) {
        const XMVECTOR world = XMVector3TransformCoord(local, rotation) + center;
        const float y = XMVectorGetY(world);
        if (y < 0.0f) {
            maxPenetration = std::max(maxPenetration, -y);
            contactSum = XMVectorAdd(contactSum, world);
            ++contactCount;
        }
    }
    if (contactCount == 0) {
        return;
    }

    const XMVECTOR contact = XMVectorScale(contactSum, 1.0f / float(contactCount));
    const XMVECTOR r = contact - center;

    if (maxPenetration > 0.0f) {
        body.position.y += maxPenetration;
        center = XMLoadFloat3(&body.position);
    }

    const XMVECTOR normal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR pointVelocity = velocity + XMVector3Cross(omega, r);
    const float normalVelocity = XMVectorGetY(pointVelocity);

    if (normalVelocity < 0.0f) {
        const XMVECTOR rn = XMVector3Cross(r, normal);
        const XMMATRIX invInertia = InverseInertiaWorld(body);
        const float denominator =
            body.invMass +
            XMVectorGetX(XMVector3Dot(rn, XMVector3TransformNormal(rn, invInertia)));
        const float impulse =
            -(1.0f + body.restitution) * normalVelocity / std::max(denominator, 1e-6f);
        if (impulse > 0.0f) {
            velocity += normal * (impulse * body.invMass);
            omega += XMVector3TransformNormal(rn * impulse, invInertia);

            XMVECTOR tangential = pointVelocity - normal * normalVelocity;
            const float tangentialSpeed = XMVectorGetX(XMVector3Length(tangential));
            if (tangentialSpeed > 1e-4f) {
                const XMVECTOR t = XMVectorScale(tangential, 1.0f / tangentialSpeed);
                float frictionImpulse = -tangentialSpeed / body.invMass;
                const float maxFriction = body.friction * impulse;
                frictionImpulse =
                    std::clamp(frictionImpulse, -maxFriction, maxFriction);
                velocity += t * (frictionImpulse * body.invMass);
            }
        }
    }

    omega = XMVectorScale(omega, 0.90f);
    velocity = XMVectorSet(XMVectorGetX(velocity) * 0.995f, XMVectorGetY(velocity),
                           XMVectorGetZ(velocity) * 0.995f, 0.0f);

    const float speed = XMVectorGetX(XMVector3Length(velocity));
    if (speed > 80.0f) {
        velocity = XMVectorScale(velocity, 80.0f / speed);
    }
    const float spin = XMVectorGetX(XMVector3Length(omega));
    if (spin > 40.0f) {
        omega = XMVectorScale(omega, 40.0f / spin);
    }

    XMStoreFloat3(&body.velocity, velocity);
    XMStoreFloat3(&body.angularVelocity, omega);
}

void PhysicsWorld::ResolvePair(int indexA, int indexB) {
    RigidBody& a = bodies_[indexA];
    RigidBody& b = bodies_[indexB];
    if (a.isStatic && b.isStatic) {
        return;
    }
    const XMVECTOR delta = XMLoadFloat3(&b.position) - XMLoadFloat3(&a.position);
    const float distance = XMVectorGetX(XMVector3Length(delta));
    if (distance > a.boundingRadius + b.boundingRadius) {
        return;
    }

    const Obb obbA = MakeObb(a);
    const Obb obbB = MakeObb(b);

    float minOverlap = FLT_MAX;
    XMVECTOR bestAxis = XMVectorZero();
    if (!TestAxis(obbA.axes[0], obbA, obbB, minOverlap, bestAxis) ||
        !TestAxis(obbA.axes[1], obbA, obbB, minOverlap, bestAxis) ||
        !TestAxis(obbA.axes[2], obbA, obbB, minOverlap, bestAxis) ||
        !TestAxis(obbB.axes[0], obbA, obbB, minOverlap, bestAxis) ||
        !TestAxis(obbB.axes[1], obbA, obbB, minOverlap, bestAxis) ||
        !TestAxis(obbB.axes[2], obbA, obbB, minOverlap, bestAxis)) {
        return;
    }

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const XMVECTOR axis = XMVector3Cross(obbA.axes[i], obbB.axes[j]);
            if (!TestAxis(axis, obbA, obbB, minOverlap, bestAxis)) {
                return;
            }
        }
    }

    if (XMVectorGetX(XMVector3Dot(bestAxis, obbB.center - obbA.center)) < 0.0f) {
        bestAxis = -bestAxis;
    }

    Manifold manifold;
    BuildManifold(obbA, obbB, bestAxis, minOverlap, manifold);
    if (manifold.count == 0) {
        return;
    }

    {
        ShockContact contact;
        contact.bodyA = indexA;
        contact.bodyB = indexB;
        XMStoreFloat3(&contact.normal, bestAxis);
        contact.count = std::min(manifold.count, 4);
        float height = 0.0f;
        for (int i = 0; i < contact.count; ++i) {
            XMStoreFloat3(&contact.points[i], manifold.points[i]);
            height += XMVectorGetY(manifold.points[i]);
        }
        contact.height = height / float(contact.count);
        contact.friction = std::sqrt(a.friction * b.friction);
        shockContacts_.push_back(contact);
    }

    const float totalInv = a.invMass + b.invMass;
    if (totalInv <= 1e-6f) {
        return;
    }

    const float restitution = std::min(a.restitution, b.restitution);
    const XMMATRIX invInertiaA = InverseInertiaWorld(a);
    const XMMATRIX invInertiaB = InverseInertiaWorld(b);

    XMVECTOR velocityA = XMLoadFloat3(&a.velocity);
    XMVECTOR omegaA = XMLoadFloat3(&a.angularVelocity);
    XMVECTOR velocityB = XMLoadFloat3(&b.velocity);
    XMVECTOR omegaB = XMLoadFloat3(&b.angularVelocity);
    XMVECTOR positionA = XMLoadFloat3(&a.position);
    XMVECTOR positionB = XMLoadFloat3(&b.position);
    float totalImpulse = 0.0f;

    for (int iteration = 0; iteration < 6; ++iteration) {
        for (int point = 0; point < manifold.count; ++point) {
            const XMVECTOR rA = manifold.points[point] - positionA;
            const XMVECTOR rB = manifold.points[point] - positionB;
            const XMVECTOR pointVelocityA = velocityA + XMVector3Cross(omegaA, rA);
            const XMVECTOR pointVelocityB = velocityB + XMVector3Cross(omegaB, rB);
            const XMVECTOR relative = pointVelocityB - pointVelocityA;
            const float normalVelocity =
                XMVectorGetX(XMVector3Dot(relative, bestAxis));
            if (normalVelocity >= 0.0f) {
                continue;
            }

            const XMVECTOR rnA = XMVector3Cross(rA, bestAxis);
            const XMVECTOR rnB = XMVector3Cross(rB, bestAxis);
            const float denominator =
                a.invMass + b.invMass +
                XMVectorGetX(XMVector3Dot(
                    rnA, XMVector3TransformNormal(rnA, invInertiaA))) +
                XMVectorGetX(XMVector3Dot(
                    rnB, XMVector3TransformNormal(rnB, invInertiaB)));
            const float impulse =
                -(1.0f + restitution) * normalVelocity / std::max(denominator, 1e-6f);
            if (impulse <= 0.0f) {
                continue;
            }

            const XMVECTOR normalImpulse = bestAxis * impulse;
            totalImpulse += impulse;
            velocityA -= normalImpulse * a.invMass;
            omegaA -= XMVector3TransformNormal(rnA * impulse, invInertiaA);
            velocityB += normalImpulse * b.invMass;
            omegaB += XMVector3TransformNormal(rnB * impulse, invInertiaB);

            XMVECTOR tangential = relative - bestAxis * normalVelocity;
            const float tangentialSpeed = XMVectorGetX(XMVector3Length(tangential));
            if (tangentialSpeed > 1e-4f) {
                const XMVECTOR t = XMVectorScale(tangential, 1.0f / tangentialSpeed);
                const XMVECTOR rtA = XMVector3Cross(rA, t);
                const XMVECTOR rtB = XMVector3Cross(rB, t);
                const float denominatorT =
                    a.invMass + b.invMass +
                    XMVectorGetX(XMVector3Dot(
                        rtA, XMVector3TransformNormal(rtA, invInertiaA))) +
                    XMVectorGetX(XMVector3Dot(
                        rtB, XMVector3TransformNormal(rtB, invInertiaB)));
                float frictionImpulse =
                    -tangentialSpeed / std::max(denominatorT, 1e-6f);
                const float maxFriction =
                    std::max(a.friction, b.friction) * impulse;
                frictionImpulse =
                    std::clamp(frictionImpulse, -maxFriction, maxFriction);
                velocityA -= t * (frictionImpulse * a.invMass);
                omegaA -= XMVector3TransformNormal(rtA * frictionImpulse, invInertiaA);
                velocityB += t * (frictionImpulse * b.invMass);
                omegaB += XMVector3TransformNormal(rtB * frictionImpulse, invInertiaB);
            }
        }
    }

    const XMVECTOR correction = bestAxis * (minOverlap * 0.4f / totalInv);
    positionA -= correction * a.invMass;
    positionB += correction * b.invMass;

    XMStoreFloat3(&a.velocity, velocityA);
    XMStoreFloat3(&a.angularVelocity, omegaA);
    XMStoreFloat3(&b.velocity, velocityB);
    XMStoreFloat3(&b.angularVelocity, omegaB);
    XMStoreFloat3(&a.position, positionA);
    XMStoreFloat3(&b.position, positionB);

    if (totalImpulse > kContactEventImpulse) {
        ContactEvent event;
        event.bodyA = indexA;
        event.bodyB = indexB;
        event.impulse = totalImpulse;
        XMStoreFloat3(&event.position, manifold.points[0]);
        contactEvents_.push_back(event);
    }

    a.sleeping = false;
    a.sleepTimer = 0.0f;
    b.sleeping = false;
    b.sleepTimer = 0.0f;
}

} // namespace ex::scene
