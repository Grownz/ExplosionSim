#include "scene/PhysicsWorld.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace ex::scene {

namespace {

constexpr float kGravity = 9.80665f;

float VectorLength(const XMFLOAT3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
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

    // Collect the penetrating corners. Using their average as the contact
    // point keeps a flat box stable while still allowing a tilted box to tip.
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

            // Linear-only Coulomb friction; angular friction is replaced by the
            // contact damping below, which is much more stable for a single
            // averaged contact point.
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

    // Contact damping removes the residual rocking of the averaged contact.
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
    const XMVECTOR positionA = XMLoadFloat3(&a.position);
    const XMVECTOR positionB = XMLoadFloat3(&b.position);
    const XMVECTOR delta = positionB - positionA;
    const float distance = XMVectorGetX(XMVector3Length(delta));
    const float radius = a.boundingRadius + b.boundingRadius;
    if (distance >= radius || distance < 1e-4f) {
        return;
    }

    const XMVECTOR normal = XMVectorScale(delta, 1.0f / distance);
    const float penetration = radius - distance;
    const float totalInv = a.invMass + b.invMass;
    if (totalInv <= 1e-6f) {
        return;
    }

    const XMVECTOR correction = XMVectorScale(normal, penetration * 0.5f / totalInv);
    XMStoreFloat3(&a.position, positionA - XMVectorScale(correction, a.invMass));
    XMStoreFloat3(&b.position, positionB + XMVectorScale(correction, b.invMass));

    const XMVECTOR velocityA = XMLoadFloat3(&a.velocity);
    const XMVECTOR velocityB = XMLoadFloat3(&b.velocity);
    const XMVECTOR relative = velocityB - velocityA;
    const float normalVelocity = XMVectorGetX(XMVector3Dot(relative, normal));

    if (normalVelocity < 0.0f) {
        const float restitution = std::min(a.restitution, b.restitution);
        const float impulse = -(1.0f + restitution) * normalVelocity / totalInv;
        const XMVECTOR impulseVector = normal * impulse;
        XMStoreFloat3(&a.velocity, velocityA - XMVectorScale(impulseVector, a.invMass));
        XMStoreFloat3(&b.velocity, velocityB + XMVectorScale(impulseVector, b.invMass));

        XMVECTOR tangential = relative - normal * normalVelocity;
        const float tangentialSpeed = XMVectorGetX(XMVector3Length(tangential));
        if (tangentialSpeed > 1e-4f) {
            const XMVECTOR t = XMVectorScale(tangential, 1.0f / tangentialSpeed);
            float frictionImpulse = -tangentialSpeed / totalInv;
            const float maxFriction = std::max(a.friction, b.friction) * impulse;
            frictionImpulse =
                std::clamp(frictionImpulse, -maxFriction, maxFriction);
            XMStoreFloat3(&a.velocity,
                          XMLoadFloat3(&a.velocity) -
                              XMVectorScale(t, frictionImpulse * a.invMass));
            XMStoreFloat3(&b.velocity,
                          XMLoadFloat3(&b.velocity) +
                              XMVectorScale(t, frictionImpulse * b.invMass));
        }
    }

    a.sleeping = false;
    a.sleepTimer = 0.0f;
    b.sleeping = false;
    b.sleepTimer = 0.0f;
}

} // namespace ex::scene
