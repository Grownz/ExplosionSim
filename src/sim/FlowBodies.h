#pragma once

#include <DirectXMath.h>

#include <vector>

#include "scene/PhysicsWorld.h"

namespace ex::sim {

// One rigid body the high-fidelity solver samples the fluid at. The solver
// uploads only `position`; the CPU keeps the rest to turn the sampled flow into
// a drag impulse. `physicsIndex` addresses PhysicsWorld::Bodies().
struct FlowBody {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 velocity{};
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    int physicsIndex = -1;
};

// Fluid state sampled at the body centre: density and velocity.
struct BodyFlowSample {
    DirectX::XMFLOAT3 velocity{};
    float density = 0.0f;
};

// Bluff-body drag coefficient used for the 4C coupling.
inline constexpr float kBodyDragCoefficient = 1.0f;

// Projected area of the oriented box along a unit direction.
float ProjectedArea(const FlowBody& body,
                    const DirectX::XMFLOAT3& direction);

// Turns the sampled flow into drag impulses:
//   F = 0.5 * rho * C_d * A * |u - v| * (u - v)
// Bodies outside the list or below a small relative speed are skipped, and a
// sleeping body is only woken when the impulse is a noticeable fraction of its
// weight, so settled rubble does not twitch in a light breeze.
void ApplyBodyDrag(scene::PhysicsWorld& physics,
                   const std::vector<FlowBody>& bodies,
                   const std::vector<BodyFlowSample>& samples, float dt);

} // namespace ex::sim
