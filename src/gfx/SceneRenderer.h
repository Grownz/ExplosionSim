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

    void Render(ID3D11DeviceContext* context, const scene::Scene& scene,
                ID3D11RenderTargetView* hdrRTV, ID3D11DepthStencilView* depthDSV);

private:
    struct Instance {
        DirectX::XMFLOAT4X4 transform;
        DirectX::XMFLOAT4 color;
        DirectX::XMFLOAT4 params;
        DirectX::XMFLOAT4 state;
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
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11DepthStencilState> depthState_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11BlendState> blendState_;
    std::vector<Mesh> meshes_;
    ID3D11Device* device_ = nullptr;
};

} // namespace ex::gfx
