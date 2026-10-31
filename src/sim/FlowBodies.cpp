#include "sim/FlowBodies.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace ex::sim {

float ProjectedArea(const FlowBody& body, const XMFLOAT3& direction) {
    const XMVECTOR orientation = XMLoadFloat4(&body.orientation);
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    const XMVECTOR axes[3] = {
        XMVector3Rotate(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), orientation),
        XMVector3Rotate(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), orientation),
        XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), orientation)};
    const float half[3] = {body.halfExtents.x, body.halfExtents.y,
                           body.halfExtents.z};

    // Sum the projected areas of the three faces that face the flow.
    float area = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const float alignment =
            std::abs(XMVectorGetX(XMVector3Dot(dir, axes[axis])));
        area += alignment * 4.0f * half[(axis + 1) % 3] * half[(axis + 2) % 3];
    }
    return std::max(area, 0.01f);
}

void ApplyBodyDrag(scene::PhysicsWorld& physics,
                   const std::vector<FlowBody>& bodies,
                   const std::vector<BodyFlowSample>& samples, float dt) {
    if (dt <= 0.0f) {
        return;
    }
    const std::vector<scene::RigidBody>& rigid = physics.Bodies();
    for (size_t i = 0; i < bodies.size() && i < samples.size(); ++i) {
        const FlowBody& body = bodies[i];
        const BodyFlowSample& sample = samples[i];
        if (body.physicsIndex < 0 ||
            body.physicsIndex >= int(rigid.size())) {
            continue;
        }
        const scene::RigidBody& rb = rigid[body.physicsIndex];
        if (rb.isStatic || sample.density <= 0.0f) {
            continue;
        }

        const XMFLOAT3 relative{sample.velocity.x - body.velocity.x,
                                sample.velocity.y - body.velocity.y,
                                sample.velocity.z - body.velocity.z};
        const float speedSq = relative.x * relative.x +
                              relative.y * relative.y +
                              relative.z * relative.z;
        if (speedSq < 1.0f) {
            // Below 1 m/s the coupling is negligible and would only keep
            // settled debris awake.
            continue;
        }
        const float speed = std::sqrt(speedSq);
        const XMFLOAT3 direction{relative.x / speed, relative.y / speed,
                                 relative.z / speed};
        const float area = ProjectedArea(body, direction);
        float force = 0.5f * sample.density * kBodyDragCoefficient * area *
                      speedSq;
        // Cap at 400 g so a numerical glitch cannot launch a body.
        force = std::min(force, 400.0f * rb.mass);
        const float impulse = force * dt;

        // Wake a sleeping body only if the drag is a noticeable fraction of
        // its weight; otherwise a light breeze re-activates the rubble pile
        // every frame.
        if (rb.sleeping && impulse < 0.05f * rb.mass * 9.80665f * dt) {
            continue;
        }
        physics.AddImpulse(
            body.physicsIndex,
            XMFLOAT3(direction.x * impulse, direction.y * impulse,
                     direction.z * impulse));
    }
}

} // namespace ex::sim
