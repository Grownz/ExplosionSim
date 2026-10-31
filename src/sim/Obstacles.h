#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "sim/VolumeBaker.h"

namespace ex::scene {
class Scene;
}

namespace ex::sim {

// One solid box handed to the high-fidelity solver and the volume bake.
// `orientation` is a unit quaternion; static objects use the identity and a
// zero velocity.
struct ObstacleBox {
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// Voxelizes oriented boxes into a dense R32F mask (1 = solid, 0 = fluid) that
// matches the volume domain. The mask resolution is independent of the solver
// or volume grid; the shaders and the CPU bake scale cell indices. Boxes are
// expanded by half a cell so thin objects do not fall between samples.
std::vector<float> VoxelizeObstacles(const std::vector<ObstacleBox>& boxes,
                                     const VolumeBounds& bounds,
                                     int resolution);

// Collects the intact buildings and vehicles of the procedural scene as boxes.
std::vector<ObstacleBox> SceneObstacles(const scene::Scene& scene);

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
