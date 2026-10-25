#include "scene/PhysicsWorld.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace ex::scene {

namespace {

constexpr float kGravity = 9.80665f;

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
    const float inertia =
        0.4f * copy.mass * copy.boundingRadius * copy.boundingRadius;
    copy.invInertia = 1.0f / std::max(inertia, 1e-3f);
    bodies_.push_back(copy);
    return int(bodies_.size()) - 1;
}

void PhysicsWorld::Step(float dt) {
    if (dt <= 0.0f) {
        return;
    }

    const XMVECTOR gravity = XMVectorSet(0.0f, -kGravity, 0.0f, 0.0f);

    for (RigidBody& body : bodies_) {
        if (body.sleeping) {
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

    for (size_t i = 0; i < bodies_.size(); ++i) {
        for (size_t j = i + 1; j < bodies_.size(); ++j) {
            if (bodies_[i].sleeping && bodies_[j].sleeping) {
                continue;
            }
            ResolvePair(bodies_[i], bodies_[j]);
        }
    }

    for (RigidBody& body : bodies_) {
        if (body.sleeping) {
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

void PhysicsWorld::ResolveGround(RigidBody& body, float dt) {
    (void)dt;

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
        const float denominator =
            body.invMass + body.invInertia * XMVectorGetX(XMVector3Dot(rn, rn));
        const float impulse =
            -(1.0f + body.restitution) * normalVelocity / std::max(denominator, 1e-6f);
        if (impulse > 0.0f) {
            velocity += normal * (impulse * body.invMass);
            omega += rn * (impulse * body.invInertia);

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

void PhysicsWorld::ResolvePair(RigidBody& a, RigidBody& b) {
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

    const float totalInv = a.invMass + b.invMass;
    if (totalInv <= 1e-6f) {
        return;
    }

    const float restitution = std::min(a.restitution, b.restitution);

    XMVECTOR velocityA = XMLoadFloat3(&a.velocity);
    XMVECTOR omegaA = XMLoadFloat3(&a.angularVelocity);
    XMVECTOR velocityB = XMLoadFloat3(&b.velocity);
    XMVECTOR omegaB = XMLoadFloat3(&b.angularVelocity);
    XMVECTOR positionA = XMLoadFloat3(&a.position);
    XMVECTOR positionB = XMLoadFloat3(&b.position);

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
                a.invInertia * XMVectorGetX(XMVector3Dot(rnA, rnA)) +
                b.invInertia * XMVectorGetX(XMVector3Dot(rnB, rnB));
            const float impulse =
                -(1.0f + restitution) * normalVelocity / std::max(denominator, 1e-6f);
            if (impulse <= 0.0f) {
                continue;
            }

            const XMVECTOR normalImpulse = bestAxis * impulse;
            velocityA -= normalImpulse * a.invMass;
            omegaA -= rnA * (impulse * a.invInertia);
            velocityB += normalImpulse * b.invMass;
            omegaB += rnB * (impulse * b.invInertia);

            XMVECTOR tangential = relative - bestAxis * normalVelocity;
            const float tangentialSpeed = XMVectorGetX(XMVector3Length(tangential));
            if (tangentialSpeed > 1e-4f) {
                const XMVECTOR t = XMVectorScale(tangential, 1.0f / tangentialSpeed);
                const XMVECTOR rtA = XMVector3Cross(rA, t);
                const XMVECTOR rtB = XMVector3Cross(rB, t);
                const float denominatorT =
                    a.invMass + b.invMass +
                    a.invInertia * XMVectorGetX(XMVector3Dot(rtA, rtA)) +
                    b.invInertia * XMVectorGetX(XMVector3Dot(rtB, rtB));
                float frictionImpulse =
                    -tangentialSpeed / std::max(denominatorT, 1e-6f);
                const float maxFriction =
                    std::max(a.friction, b.friction) * impulse;
                frictionImpulse =
                    std::clamp(frictionImpulse, -maxFriction, maxFriction);
                velocityA -= t * (frictionImpulse * a.invMass);
                omegaA -= rtA * (frictionImpulse * a.invInertia);
                velocityB += t * (frictionImpulse * b.invMass);
                omegaB += rtB * (frictionImpulse * b.invInertia);
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

    a.sleeping = false;
    a.sleepTimer = 0.0f;
    b.sleeping = false;
    b.sleepTimer = 0.0f;
}

} // namespace ex::scene
