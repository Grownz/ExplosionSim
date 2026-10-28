#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <cstdint>
#include <vector>

#include "gfx/GfxDevice.h"
#include "scene/SceneTypes.h"

namespace ex::gfx {

// Draws the procedural environment as instanced, procedurally generated meshes.
// The pass writes depth before the volumetric pass, so buildings and trees
// correctly occlude the fireball/smoke.
class SceneRenderer {
public:
    struct Vertex {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT3 color;
    };

    bool Initialize(ID3D11Device* device);
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
    void RenderStaticShadow(ID3D11DeviceContext* context,
                            const scene::Scene& scene,
                            const DirectX::XMFLOAT4X4& lightViewProj,
                            uint64_t key);

    // Renders the scene into the cascaded directional shadow maps and clears
    // them when the scene is disabled. Must run before the ground pass.
    void RenderShadow(ID3D11DeviceContext* context, const scene::Scene& scene,
                      const ViewInfo& view,
                      const DirectX::XMFLOAT4X4* lightViewProj,
                      const DirectX::XMFLOAT3& burstCenter);

    // Main opaque scene pass. Must run after the ground pass and before the
    // depth snapshot / volumetric pass.
    void RenderScene(ID3D11DeviceContext* context, const scene::Scene& scene,
                     ID3D11RenderTargetView* hdrRTV,
                     ID3D11DepthStencilView* depthDSV, int width, int height,
                     ID3D11SamplerState* surfaceSampler);

    ID3D11ShaderResourceView* ShadowSRV(int cascade) const {
        return shadowSRV_[cascade].Get();
    }
    ID3D11ShaderResourceView* StaticShadowSRV() const {
        return staticShadowSRV_.Get();
    }
    ID3D11SamplerState* ShadowSampler() const { return shadowSampler_.Get(); }

private:
    bool CreateShadowResources(ID3D11Device* device);
    bool CreateSurfaceDetailTexture(ID3D11Device* device);
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
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
        ComPtr<ID3D11Buffer> instanceBuffer;
        ComPtr<ID3D11ShaderResourceView> instanceSRV;
        UINT indexCount = 0;
        UINT instanceCapacity = 0;
        std::vector<Instance> instances;
    };

    bool CreateMesh(const std::vector<Vertex>& vertices,
                    const std::vector<uint32_t>& indices, Mesh& mesh);
    bool EnsureInstanceCapacity(Mesh& mesh, UINT capacity);
    void BuildMeshes();

    bool BuildChunkGeometry(const scene::Scene& scene);
    bool EnsureChunkInstanceCapacity(UINT capacity);
    void UpdateChunkInstances(ID3D11DeviceContext* context,
                              const scene::Scene& scene);
    void DrawChunks(ID3D11DeviceContext* context, const scene::Scene& scene,
                    bool shadowPass);
    bool EnsureStaticInstanceCapacity(UINT capacity);

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11VertexShader> shadowVertexShader_;
    ComPtr<ID3D11VertexShader> chunkVertexShader_;
    ComPtr<ID3D11VertexShader> chunkShadowVertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11InputLayout> chunkInputLayout_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Texture2D> shadowTexture_[kShadowCascades];
    ComPtr<ID3D11DepthStencilView> shadowDSV_[kShadowCascades];
    ComPtr<ID3D11ShaderResourceView> shadowSRV_[kShadowCascades];
    ComPtr<ID3D11Buffer> shadowCascadeBuffer_;
    ComPtr<ID3D11SamplerState> shadowSampler_;
    // Tiling high-frequency grain sampled with the selected anisotropic filter.
    ComPtr<ID3D11Texture2D> surfaceDetailTexture_;
    ComPtr<ID3D11ShaderResourceView> surfaceDetailSRV_;
    std::vector<Mesh> meshes_;

    // Cached static-caster shadow map (intact buildings, world space).
    ComPtr<ID3D11Texture2D> staticShadowTexture_;
    ComPtr<ID3D11DepthStencilView> staticShadowDSV_;
    ComPtr<ID3D11ShaderResourceView> staticShadowSRV_;
    ComPtr<ID3D11Buffer> staticInstanceBuffer_;
    ComPtr<ID3D11ShaderResourceView> staticInstanceSRV_;
    UINT staticInstanceCapacity_ = 0;
    uint64_t staticShadowKey_ = 0;
    bool staticShadowValid_ = false;

    // Voronoi chunk geometry and per-frame instance data.
    ComPtr<ID3D11Buffer> chunkVertexBuffer_;
    ComPtr<ID3D11Buffer> chunkIndexBuffer_;
    ComPtr<ID3D11Buffer> chunkInstanceBuffer_;
    ComPtr<ID3D11ShaderResourceView> chunkInstanceSRV_;
    std::vector<ChunkDraw> chunkDraws_;
    std::vector<Instance> chunkInstances_;
    UINT chunkInstanceCount_ = 0;
    UINT chunkInstanceCapacity_ = 0;
    uint64_t chunkGeometryKey_ = 0;
    bool chunkGeometryValid_ = false;

    ID3D11Device* device_ = nullptr;
};

} // namespace ex::gfx
