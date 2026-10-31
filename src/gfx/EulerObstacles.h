#pragma once

#include <DirectXMath.h>

#include <vector>

#include "sim/VolumeBaker.h"

namespace ex::scene {
class Scene;
}

namespace ex::gfx {

// One solid box handed to the high-fidelity solver. `orientation` is a unit
// quaternion; static objects use the identity and a zero velocity.
struct ObstacleBox {
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// Voxelizes oriented boxes into a dense R32F mask (1 = solid, 0 = fluid) that
// matches the solver domain. The mask resolution is independent of the solver
// grid; the shaders scale cell indices. Boxes are expanded by half a cell so
// thin objects do not fall between samples.
std::vector<float> VoxelizeObstacles(const std::vector<ObstacleBox>& boxes,
                                     const sim::VolumeBounds& bounds,
                                     int resolution);

// Collects the intact buildings and vehicles of the procedural scene as boxes.
std::vector<ObstacleBox> SceneObstacles(const scene::Scene& scene);

} // namespace ex::gfx
