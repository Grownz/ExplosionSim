#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <cstdint>
#include <vector>

#include "gfx/GfxDevice.h"
#include "gfx/SceneMeshLibrary.h"
#include "scene/SceneTypes.h"
#include "scene/VehicleModels.h"

namespace ex::gfx {

// Draws the procedural environment as instanced, procedurally generated meshes.
// The pass writes depth before the volumetric pass, so buildings and trees
// correctly occlude the fireball/smoke.
class SceneRenderer {
public:
    using Vertex = SceneMeshVertex;

    bool Initialize(rhi::Device* rhiDevice);
    void Shutdown();

    // Camera data used for culling and level of detail.
    struct ViewInfo {
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT3 eye{};
        float projScale = 1.0f; // pixels per world unit at one metre
    };

    static constexpr int kShadowCascades = 3;
    static int ShadowSize(int cascade) {
        return cascade == 0 ? 2048 : 1024;
    }
    // World-space map for the cached static casters. It is independent of the
    // camera, so it only has to be re-rendered when the scene or sun changes.
    static constexpr int kStaticShadowSize = 4096;

    // Renders the intact buildings into the cached static shadow map. Skips the
    // work when `key` matches the previous call.
    void RenderStaticShadow(rhi::CommandList* commandList,
                            const scene::Scene& scene,
                            const DirectX::XMFLOAT4X4& lightViewProj,
                            uint64_t key);

    // Renders the scene into the cascaded directional shadow maps and clears
    // them when the scene is disabled. Must run before the ground pass.
    void RenderShadow(rhi::CommandList* commandList, const scene::Scene& scene,
                      const ViewInfo& view,
                      const DirectX::XMFLOAT4X4* lightViewProj,
                      const DirectX::XMFLOAT3& burstCenter);

    // Main opaque scene pass. Must run after the ground pass and before the
    // depth snapshot / volumetric pass.
    void RenderScene(rhi::CommandList* commandList, const scene::Scene& scene,
                     rhi::Texture* hdrTarget, rhi::Texture* depthTarget,
                     rhi::Sampler* surfaceSampler);

    ID3D11ShaderResourceView* ShadowSRV(int cascade) const {
        return shadowTexture_[cascade]
                   ? static_cast<ID3D11ShaderResourceView*>(
                         shadowTexture_[cascade]->NativeShaderResourceView())
                   : nullptr;
    }
    ID3D11ShaderResourceView* StaticShadowSRV() const {
        return staticShadowTexture_
                   ? static_cast<ID3D11ShaderResourceView*>(
                         staticShadowTexture_->NativeShaderResourceView())
                   : nullptr;
    }
    ID3D11SamplerState* ShadowSampler() const {
        return shadowSampler_
                   ? static_cast<ID3D11SamplerState*>(shadowSampler_->NativeHandle())
                   : nullptr;
    }

private:
    bool CreateShadowResources();
    bool CreateSurfaceDetailTexture();
    struct Instance {
        DirectX::XMFLOAT4X4 transform;
        DirectX::XMFLOAT4 color;
        DirectX::XMFLOAT4 params;
        DirectX::XMFLOAT4 state;
        // Maps the mesh-local position back to the original building space so
        // rubble keeps the facade of the part it broke off from.
        DirectX::XMFLOAT4 textureOrigin;
        DirectX::XMFLOAT4 textureScale;
    };

    // Voronoi chunks: one global vertex/index buffer with the convex hulls in
    // building space and one instance per chunk. Vertices carry the chunk id so
    // a building is drawn with a single non-instanced draw call.
    struct ChunkVertex {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        uint32_t chunkId;
    };

    struct ChunkDraw {
        UINT indexOffset = 0;
        UINT indexCount = 0;
        int objectIndex = -1; // only drawn once the building is collapsing

    };

    struct Mesh {
        std::unique_ptr<rhi::Buffer> vertexBuffer;
        std::unique_ptr<rhi::Buffer> indexBuffer;
        std::unique_ptr<rhi::Buffer> instanceBuffer;
        UINT indexCount = 0;
        UINT instanceCapacity = 0;
        // Natural bounds of imported vehicle meshes, used to scale them to the
        // scene object size without distorting their proportions.
        DirectX::XMFLOAT3 modelSize{};
        std::vector<Instance> instances;
    };

    // Mesh slots. Imported vehicles occupy 1..kVehicleModelCount; the tree,
    // chunk and far-tree meshes follow them.
    static constexpr int kVehicleMeshFirst = 1;
    static constexpr int kTreeMeshFirst = kVehicleMeshFirst + scene::kVehicleModelCount;
    static constexpr int kChunkMesh = kTreeMeshFirst + 3;
    static constexpr int kFarTreeMesh = kChunkMesh + 1;
    static constexpr int kMeshCount = kFarTreeMesh + 1;

    bool CreateMesh(const std::vector<Vertex>& vertices,
                    const std::vector<uint32_t>& indices, Mesh& mesh);
    bool LoadVehicleMesh(const wchar_t* asset, Mesh& mesh);
    bool EnsureInstanceCapacity(Mesh& mesh, UINT capacity);
    void BuildMeshes();

    bool BuildChunkGeometry(const scene::Scene& scene);
    bool EnsureChunkInstanceCapacity(UINT capacity);
    void UpdateChunkInstances(const scene::Scene& scene);
    void DrawChunks(rhi::CommandList* commandList, const scene::Scene& scene,
                    bool shadowPass);
    bool EnsureStaticInstanceCapacity(UINT capacity);

    std::unique_ptr<rhi::Pipeline> sceneShadowPipeline_;
    std::unique_ptr<rhi::Pipeline> chunkShadowPipeline_;
    std::unique_ptr<rhi::Pipeline> scenePipeline_;
    std::unique_ptr<rhi::Pipeline> chunkPipeline_;
    std::unique_ptr<rhi::Texture> shadowTexture_[kShadowCascades];
    std::unique_ptr<rhi::Buffer> shadowCascadeBuffer_;
    std::unique_ptr<rhi::Sampler> shadowSampler_;
    // Tiling high-frequency grain sampled with the selected anisotropic filter.
    std::unique_ptr<rhi::Texture> surfaceDetailTexture_;
    std::vector<Mesh> meshes_;

    // Cached static-caster shadow map (intact buildings, world space).
    std::unique_ptr<rhi::Texture> staticShadowTexture_;
    std::unique_ptr<rhi::Buffer> staticInstanceBuffer_;
    UINT staticInstanceCapacity_ = 0;
    uint64_t staticShadowKey_ = 0;
    bool staticShadowValid_ = false;

    // Voronoi chunk geometry and per-frame instance data.
    std::unique_ptr<rhi::Buffer> chunkVertexBuffer_;
    std::unique_ptr<rhi::Buffer> chunkIndexBuffer_;
    std::unique_ptr<rhi::Buffer> chunkInstanceBuffer_;
    std::vector<ChunkDraw> chunkDraws_;
    std::vector<Instance> chunkInstances_;
    UINT chunkInstanceCount_ = 0;
    UINT chunkInstanceCapacity_ = 0;
    uint64_t chunkGeometryKey_ = 0;
    bool chunkGeometryValid_ = false;

    rhi::Device* rhiDevice_ = nullptr;
};

} // namespace ex::gfx
