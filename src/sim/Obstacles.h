#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "scene/PhysicsWorld.h"
#include "sim/VolumeBaker.h"

namespace ex::scene {
class Scene;
}

namespace ex::sim {

// One solid box handed to the high-fidelity solver and the volume bake.
// `orientation` is a unit quaternion, `velocity` the wall velocity used by the
// no-slip mirror (debris and shoved objects; static scene boxes use zero).
struct ObstacleBox {
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 velocity{};
};

// Voxelizes oriented boxes into a dense R32F mask (1 = solid, 0 = fluid) that
// matches the volume domain. The mask resolution is independent of the solver
// or volume grid; the shaders and the CPU bake scale cell indices. Boxes are
// expanded by half a cell so thin objects do not fall between samples.
std::vector<float> VoxelizeObstacles(const std::vector<ObstacleBox>& boxes,
                                     const VolumeBounds& bounds,
                                     int resolution);

// Same voxelization, but for the Euler solver: RGBA16F with the solid
// velocity in rgb and the solid flag in alpha. Later boxes overwrite earlier
// ones, so callers append the moving physics bodies after the static scene
// boxes.
std::vector<uint16_t> VoxelizeObstacleVelocityMask(
    const std::vector<ObstacleBox>& boxes, const VolumeBounds& bounds,
    int resolution);

// Collects the intact buildings and vehicles of the procedural scene as boxes.
std::vector<ObstacleBox> SceneObstacles(const scene::Scene& scene);

// Collects the rigid bodies that overlap `bounds` (debris, shoved objects and
// the remaining structure) with their current velocity. The solver wall moves
// with them; bodies outside the solver domain are skipped.
std::vector<ObstacleBox> PhysicsObstacles(const scene::PhysicsWorld& physics,
                                          const VolumeBounds& bounds);

// Exact comparison used to skip a mask upload when nothing moved.
bool SameObstacles(const std::vector<ObstacleBox>& a,
                   const std::vector<ObstacleBox>& b);

// Caches the solid mask for the volume bake. The mask only depends on the
// obstacle boxes, the bake box and the mask resolution, so it is rebuilt only
// when one of them changes instead of on every bake. The shared pointer keeps
// the previous mask alive for an asynchronous CPU bake, and `Version()` lets the
// GPU paths skip re-uploading an unchanged mask.
class VolumeObstacleMask {
public:
    const std::shared_ptr<const std::vector<float>>& Update(
        const scene::Scene& scene, const VolumeBounds& wanted, int resolution);

    const std::shared_ptr<const std::vector<float>>& Mask() const {
        return mask_;
    }
    // World-space box the mask was voxelized over. It always contains the
    // requested bake box, so the mask stays valid while the box drifts or grows.
    const VolumeBounds& Bounds() const { return bounds_; }
    int Resolution() const { return resolution_; }
    uint64_t Version() const { return version_; }

private:
    std::vector<ObstacleBox> obstacles_;
    VolumeBounds bounds_{};
    int resolution_ = 0;
    uint64_t version_ = 0;
    std::shared_ptr<const std::vector<float>> mask_;
};

} // namespace ex::sim
