#include "sim/Obstacles.h"

#include "scene/SceneTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace DirectX;

namespace ex::sim {

std::vector<float> VoxelizeObstacles(const std::vector<ObstacleBox>& boxes,
                                     const VolumeBounds& bounds,
                                     int resolution) {
    const int res = std::clamp(resolution, 8, 256);
    std::vector<float> mask(size_t(res) * size_t(res) * size_t(res), 0.0f);
    if (boxes.empty()) {
        return mask;
    }

    const XMFLOAT3 extent{bounds.max.x - bounds.min.x,
                          bounds.max.y - bounds.min.y,
                          bounds.max.z - bounds.min.z};
    const float cellX = extent.x / float(res);
    const float cellY = extent.y / float(res);
    const float cellZ = extent.z / float(res);
    if (cellX <= 0.0f || cellY <= 0.0f || cellZ <= 0.0f) {
        return mask;
    }

    for (const ObstacleBox& box : boxes) {
        const XMVECTOR center = XMLoadFloat3(&box.center);
        const XMVECTOR orientation = XMLoadFloat4(&box.orientation);
        const XMVECTOR half = XMLoadFloat3(&box.halfExtents);
        const float radius =
            XMVectorGetX(XMVector3Length(half)) + 0.5f * std::max({cellX, cellY, cellZ});

        // Conservative world-space bounds of the oriented box.
        const float minX = box.center.x - radius;
        const float maxX = box.center.x + radius;
        const float minY = box.center.y - radius;
        const float maxY = box.center.y + radius;
        const float minZ = box.center.z - radius;
        const float maxZ = box.center.z + radius;

        const int x0 = std::max(0, int(std::floor((minX - bounds.min.x) / cellX)));
        const int x1 = std::min(res - 1, int(std::ceil((maxX - bounds.min.x) / cellX)));
        const int y0 = std::max(0, int(std::floor((minY - bounds.min.y) / cellY)));
        const int y1 = std::min(res - 1, int(std::ceil((maxY - bounds.min.y) / cellY)));
        const int z0 = std::max(0, int(std::floor((minZ - bounds.min.z) / cellZ)));
        const int z1 = std::min(res - 1, int(std::ceil((maxZ - bounds.min.z) / cellZ)));
        if (x0 > x1 || y0 > y1 || z0 > z1) {
            continue;
        }

        const XMFLOAT3 halfCell{0.5f * cellX, 0.5f * cellY, 0.5f * cellZ};
        for (int z = z0; z <= z1; ++z) {
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const XMFLOAT3 world{
                        bounds.min.x + (float(x) + 0.5f) * cellX,
                        bounds.min.y + (float(y) + 0.5f) * cellY,
                        bounds.min.z + (float(z) + 0.5f) * cellZ};
                    const XMVECTOR local = XMVector3InverseRotate(
                        XMLoadFloat3(&world) - center, orientation);
                    XMFLOAT3 localValue{};
                    XMStoreFloat3(&localValue, local);
                    if (std::abs(localValue.x) <= box.halfExtents.x + halfCell.x &&
                        std::abs(localValue.y) <= box.halfExtents.y + halfCell.y &&
                        std::abs(localValue.z) <= box.halfExtents.z + halfCell.z) {
                        const size_t index =
                            (size_t(z) * size_t(res) + size_t(y)) * size_t(res) +
                            size_t(x);
                        mask[index] = 1.0f;
                    }
                }
            }
        }
    }
    return mask;
}

std::vector<ObstacleBox> SceneObstacles(const scene::Scene& scene) {
    std::vector<ObstacleBox> boxes;
    for (const scene::SceneObject& object : scene.Objects()) {
        if (object.type == scene::SceneObjectType::Tree) {
            continue;
        }
        if (object.collapsed ||
            object.damage == scene::DamageState::Destroyed) {
            continue;
        }
        ObstacleBox box;
        box.center = XMFLOAT3(object.position.x,
                              object.position.y + object.size.y * 0.5f,
                              object.position.z);
        box.halfExtents = XMFLOAT3(object.size.x * 0.5f,
                                   object.size.y * 0.5f,
                                   object.size.z * 0.5f);
        const float yaw = XMConvertToRadians(object.yaw);
        XMStoreFloat4(&box.orientation,
                      XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));
        boxes.push_back(box);
    }
    return boxes;
}

namespace {

bool SameBoxes(const std::vector<ObstacleBox>& a,
               const std::vector<ObstacleBox>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const ObstacleBox& x = a[i];
        const ObstacleBox& y = b[i];
        if (x.center.x != y.center.x || x.center.y != y.center.y ||
            x.center.z != y.center.z || x.halfExtents.x != y.halfExtents.x ||
            x.halfExtents.y != y.halfExtents.y ||
            x.halfExtents.z != y.halfExtents.z ||
            x.orientation.x != y.orientation.x ||
            x.orientation.y != y.orientation.y ||
            x.orientation.z != y.orientation.z ||
            x.orientation.w != y.orientation.w) {
            return false;
        }
    }
    return true;
}

bool Contains(const VolumeBounds& outer, const VolumeBounds& inner) {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y &&
           outer.min.z <= inner.min.z && outer.max.x >= inner.max.x &&
           outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

VolumeBounds Union(const VolumeBounds& a, const VolumeBounds& b) {
    VolumeBounds result{};
    result.min.x = std::min(a.min.x, b.min.x);
    result.min.y = std::min(a.min.y, b.min.y);
    result.min.z = std::min(a.min.z, b.min.z);
    result.max.x = std::max(a.max.x, b.max.x);
    result.max.y = std::max(a.max.y, b.max.y);
    result.max.z = std::max(a.max.z, b.max.z);
    return result;
}

} // namespace

const std::shared_ptr<const std::vector<float>>& VolumeObstacleMask::Update(
    const scene::Scene& scene, const VolumeBounds& wanted, int resolution) {
    const std::vector<ObstacleBox> obstacles = SceneObstacles(scene);
    // The mask is valid as long as the requested bake box stays inside the
    // covered world-space box; the box drifts downwind between bakes, which
    // must not rebuild the mask. Rebuild only when the scene, the resolution or
    // the coverage changes.
    const bool rebuild = !mask_ || !SameBoxes(obstacles, obstacles_) ||
                         resolution != resolution_ || !Contains(bounds_, wanted);
    if (!rebuild) {
        return mask_;
    }

    obstacles_ = obstacles;
    resolution_ = resolution;
    bounds_ = mask_ ? Union(bounds_, wanted) : wanted;
    ++version_;
    if (obstacles_.empty()) {
        mask_.reset();
    } else {
        mask_ = std::make_shared<const std::vector<float>>(
            VoxelizeObstacles(obstacles_, bounds_, resolution_));
    }
    return mask_;
}

} // namespace ex::sim