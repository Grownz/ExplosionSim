#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace ex::scene {

// One convex Voronoi cell clipped to the building box. The hull is stored in
// building space with flat per-face normals; the collision proxy is an oriented
// box fitted to the cell vertices.
struct FractureCell {
    DirectX::XMFLOAT3 center{};       // collision box centre
    DirectX::XMFLOAT3 half{};         // collision box half extents
    DirectX::XMFLOAT4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    float volume = 0.0f;
    float surfaceArea = 0.0f;
    bool grounded = false;

    std::vector<DirectX::XMFLOAT3> positions; // triangulated hull
    std::vector<DirectX::XMFLOAT3> normals;   // parallel to positions
    std::vector<uint32_t> indices;

    std::vector<int> neighbors; // cells that share a face
};

// Splits an axis-aligned box into `cellCount` convex Voronoi cells. The point
// distribution is deterministic for a given `seed`, so the fracture is
// reproducible across runs. Cells are appended in seed order.
void FractureBoxVoronoi(const DirectX::XMFLOAT3& center,
                        const DirectX::XMFLOAT3& half, int cellCount,
                        uint32_t seed, std::vector<FractureCell>& cells);

} // namespace ex::scene
