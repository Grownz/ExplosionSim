#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace ex::gfx {

// Shared procedural mesh format for the scene renderer. The layout matches the
// POSITION/NORMAL/COLOR vertex input used by scene.hlsl.
struct SceneMeshVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT3 color;
};

struct SceneMeshData {
    std::vector<SceneMeshVertex> vertices;
    std::vector<uint32_t> indices;
};

// Unit building box with its base at y = 0 and a 1x1x1 footprint.
SceneMeshData MakeBuildingMesh();
// Placeholder vehicle used when an imported .esv model is unavailable.
SceneMeshData MakeVehiclePlaceholderMesh();
// Tree variants 0..2. Canonical height 1, canopy radius 0.5.
SceneMeshData MakeTreeMesh(int variant);
// Low-detail single-cone tree used beyond the near distance.
SceneMeshData MakeFarTreeMesh();
// Chamfered unit box used for Voronoi rubble.
SceneMeshData MakeChunkMesh();

// Primitive helpers shared by the builders and the renderer.
void MeshAddBox(std::vector<SceneMeshVertex>& vertices,
            std::vector<uint32_t>& indices, const DirectX::XMFLOAT3& center,
            const DirectX::XMFLOAT3& half, const DirectX::XMFLOAT3& color);
void MeshAddCone(std::vector<SceneMeshVertex>& vertices,
             std::vector<uint32_t>& indices,
             const DirectX::XMFLOAT3& baseCenter, float radius, float height,
             const DirectX::XMFLOAT3& color, int segments = 7);
void MeshAddBeveledBox(std::vector<SceneMeshVertex>& vertices,
                   std::vector<uint32_t>& indices,
                   const DirectX::XMFLOAT3& half, float bevel,
                   const DirectX::XMFLOAT3& color);

} // namespace ex::gfx
