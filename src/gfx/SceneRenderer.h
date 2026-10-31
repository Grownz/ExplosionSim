#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "gfx/rhi/Rhi.h"
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

    // Platform-supplied shader bytecode. Windows compiles scene.hlsl with
    // D3DCompile at startup; Linux embeds the SPIR-V generated from the same
    // source. Keeping the sources outside the renderer makes it buildable and
    // usable on both backends.
    struct ShaderSource {
        const void* data = nullptr;
        size_t size = 0;
    };
    struct Shaders {
        ShaderSource sceneVertex;        // VSScene
        ShaderSource scenePixel;         // PSScene
        ShaderSource shadowVertex;       // VSShadow
        ShaderSource chunkVertex;        // VSChunk
        ShaderSource chunkShadowVertex;  // VSShadowChunk
    };

    bool Initialize(rhi::Device* rhiDevice, const Shaders& shaders);
    void Shutdown();

    static constexpr int kShadowCascades = 3;
    static int ShadowSize(int cascade) {
        return cascade == 0 ? 2048 : 1024;
    }
    // World-space map for the cached static casters. It is independent of the
    // camera, so it only has to be re-rendered when the scene or sun changes.
    static constexpr int kStaticShadowSize = 4096;

    // Camera data used for culling and level of detail.
    struct ViewInfo {
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT3 eye{};
        float projScale = 1.0f; // pixels per world unit at one metre
    };

    // Cascaded shadow setup shared by both applications. The layout mirrors the
    // ShadowConstants cbuffer in scene.hlsl and ground.hlsl.
    struct ShadowConstants {
        DirectX::XMFLOAT4X4 lightViewProj[kShadowCascades];
        DirectX::XMFLOAT4X4 staticLightViewProj;
        DirectX::XMFLOAT4 cascadeSplits;
        DirectX::XMFLOAT4 cascadeTexel;
        DirectX::XMFLOAT4 cascadeWorldTexel;
        DirectX::XMFLOAT4 staticTexel;
    };

    struct ShadowInput {
        DirectX::XMFLOAT4X4 viewMatrix{};
        DirectX::XMFLOAT3 sunDirection{0.0f, 1.0f, 0.0f};
        float nearPlane = 0.1f;
        float fovY = 1.0f;
        float cameraDistance = 10.0f;
        int viewportWidth = 1;
        int viewportHeight = 1;
        float sceneExtent = 60.0f;
    };
    static ShadowConstants ComputeShadows(const ShadowInput& input);

    // Resources owned by the caller that the scene/ground shaders sample. The
    // Windows renderer binds its native buffers before the call and passes an
    // empty struct; the Linux preview passes its RHI resources so they can be
    // bound through the Vulkan descriptor set.
    struct FrameResources {
        rhi::Buffer* frameConstants = nullptr;
        rhi::Buffer* shadowConstants = nullptr;
        rhi::Texture* volumeTexture = nullptr;
        rhi::Sampler* volumeSampler = nullptr;
    };

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
                     rhi::Sampler* surfaceSampler,
                     const FrameResources& frameResources);
    void RenderScene(rhi::CommandList* commandList, const scene::Scene& scene,
                     rhi::Texture* hdrTarget, rhi::Texture* depthTarget,
                     rhi::Sampler* surfaceSampler) {
        RenderScene(commandList, scene, hdrTarget, depthTarget, surfaceSampler,
                    FrameResources{});
    }

    // Shadow maps are exposed as RHI textures so both backends can bind them.
    // Windows' legacy ground pass derives the native D3D11 views from them.
    rhi::Texture* ShadowTexture(int cascade) const {
        return cascade >= 0 && cascade < kShadowCascades
                   ? shadowTexture_[cascade].get()
                   : nullptr;
    }
    rhi::Texture* StaticShadowTexture() const {
        return staticShadowTexture_.get();
    }
    rhi::Sampler* ShadowSampler() const { return shadowSampler_.get(); }

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
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        int objectIndex = -1; // only drawn once the building is collapsing

    };

    struct Mesh {
        std::unique_ptr<rhi::Buffer> vertexBuffer;
        std::unique_ptr<rhi::Buffer> indexBuffer;
        std::unique_ptr<rhi::Buffer> instanceBuffer;
        uint32_t indexCount = 0;
        uint32_t instanceCapacity = 0;
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
    bool LoadVehicleMesh(const char* asset, Mesh& mesh);
    bool EnsureInstanceCapacity(Mesh& mesh, uint32_t capacity);
    void BuildMeshes();

    bool BuildChunkGeometry(const scene::Scene& scene);
    bool EnsureChunkInstanceCapacity(uint32_t capacity);
    void UpdateChunkInstances(const scene::Scene& scene);
    void DrawChunks(rhi::CommandList* commandList, const scene::Scene& scene,
                    bool shadowPass);
    void BindSceneResources(rhi::CommandList* commandList);
    bool EnsureStaticInstanceCapacity(uint32_t capacity);

    std::unique_ptr<rhi::Pipeline> sceneShadowPipeline_;
    std::unique_ptr<rhi::Pipeline> chunkShadowPipeline_;
    std::unique_ptr<rhi::Pipeline> scenePipeline_;
    std::unique_ptr<rhi::Pipeline> chunkPipeline_;
    std::unique_ptr<rhi::Texture> shadowTexture_[kShadowCascades];
    std::unique_ptr<rhi::Buffer> shadowCascadeBuffer_;
    std::unique_ptr<rhi::Sampler> shadowSampler_;
    // Resources bound by RenderScene and re-bound after a pipeline switch.
    FrameResources frameResources_{};
    rhi::Sampler* frameSurfaceSampler_ = nullptr;
    // Tiling high-frequency grain sampled with the selected anisotropic filter.
    std::unique_ptr<rhi::Texture> surfaceDetailTexture_;
    std::vector<Mesh> meshes_;

    // Cached static-caster shadow map (intact buildings, world space).
    std::unique_ptr<rhi::Texture> staticShadowTexture_;
    std::unique_ptr<rhi::Buffer> staticInstanceBuffer_;
    uint32_t staticInstanceCapacity_ = 0;
    uint64_t staticShadowKey_ = 0;
    bool staticShadowValid_ = false;

    // Voronoi chunk geometry and per-frame instance data.
    std::unique_ptr<rhi::Buffer> chunkVertexBuffer_;
    std::unique_ptr<rhi::Buffer> chunkIndexBuffer_;
    std::unique_ptr<rhi::Buffer> chunkInstanceBuffer_;
    std::vector<ChunkDraw> chunkDraws_;
    std::vector<Instance> chunkInstances_;
    uint32_t chunkInstanceCount_ = 0;
    uint32_t chunkInstanceCapacity_ = 0;
    uint64_t chunkGeometryKey_ = 0;
    bool chunkGeometryValid_ = false;

    rhi::Device* rhiDevice_ = nullptr;
};

} // namespace ex::gfx
