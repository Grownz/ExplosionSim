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

    // Renders the scene into the cascaded directional shadow maps and clears
    // them when the scene is disabled. Must run before the ground pass.
    void RenderShadow(ID3D11DeviceContext* context, const scene::Scene& scene,
                      const ViewInfo& view,
                      const DirectX::XMFLOAT4X4* lightViewProj);

    // Main opaque scene pass. Must run after the ground pass and before the
    // depth snapshot / volumetric pass.
    void RenderScene(ID3D11DeviceContext* context, const scene::Scene& scene,
                     ID3D11RenderTargetView* hdrRTV,
                     ID3D11DepthStencilView* depthDSV, int width, int height);

    ID3D11ShaderResourceView* ShadowSRV(int cascade) const {
        return shadowSRV_[cascade].Get();
    }
    ID3D11SamplerState* ShadowSampler() const { return shadowSampler_.Get(); }

private:
    bool CreateShadowResources(ID3D11Device* device);
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

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11VertexShader> shadowVertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Texture2D> shadowTexture_[kShadowCascades];
    ComPtr<ID3D11DepthStencilView> shadowDSV_[kShadowCascades];
    ComPtr<ID3D11ShaderResourceView> shadowSRV_[kShadowCascades];
    ComPtr<ID3D11Buffer> shadowCascadeBuffer_;
    ComPtr<ID3D11SamplerState> shadowSampler_;
    std::vector<Mesh> meshes_;
    ID3D11Device* device_ = nullptr;
};

} // namespace ex::gfx
