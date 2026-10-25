#include "gfx/SceneRenderer.h"

#include "core/Log.h"
#include "gfx/Frustum.h"
#include "gfx/Shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace ex::gfx {

namespace {

using Vertex = SceneRenderer::Vertex;

void AddTriangle(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                 const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                 const XMFLOAT3& normal, const XMFLOAT3& color) {
    const uint32_t base = uint32_t(vertices.size());
    vertices.push_back({a, normal, color});
    vertices.push_back({b, normal, color});
    vertices.push_back({c, normal, color});
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
}

void AddBox(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
            const XMFLOAT3& center, const XMFLOAT3& half, const XMFLOAT3& color) {
    auto corner = [&center, &half](float sx, float sy, float sz) {
        return XMFLOAT3(center.x + sx * half.x, center.y + sy * half.y,
                        center.z + sz * half.z);
    };

    struct Face {
        XMFLOAT3 a, b, c, d;
        XMFLOAT3 normal;
    };

    const Face faces[6] = {
        {corner(1, -1, -1), corner(1, -1, 1), corner(1, 1, 1), corner(1, 1, -1),
         {1, 0, 0}},
        {corner(-1, -1, 1), corner(-1, -1, -1), corner(-1, 1, -1), corner(-1, 1, 1),
         {-1, 0, 0}},
        {corner(-1, 1, -1), corner(1, 1, -1), corner(1, 1, 1), corner(-1, 1, 1),
         {0, 1, 0}},
        {corner(-1, -1, 1), corner(1, -1, 1), corner(1, -1, -1), corner(-1, -1, -1),
         {0, -1, 0}},
        {corner(-1, -1, 1), corner(-1, 1, 1), corner(1, 1, 1), corner(1, -1, 1),
         {0, 0, 1}},
        {corner(1, -1, -1), corner(1, 1, -1), corner(-1, 1, -1), corner(-1, -1, -1),
         {0, 0, -1}},
    };

    for (const Face& face : faces) {
        const uint32_t base = uint32_t(vertices.size());
        vertices.push_back({face.a, face.normal, color});
        vertices.push_back({face.b, face.normal, color});
        vertices.push_back({face.c, face.normal, color});
        vertices.push_back({face.d, face.normal, color});
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

void AddCone(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
             const XMFLOAT3& baseCenter, float radius, float height,
             const XMFLOAT3& color, int segments = 7) {
    const XMFLOAT3 apex(baseCenter.x, baseCenter.y + height, baseCenter.z);
    for (int i = 0; i < segments; ++i) {
        const float a0 = 6.2831853f * float(i) / float(segments);
        const float a1 = 6.2831853f * float(i + 1) / float(segments);
        const XMFLOAT3 b0(baseCenter.x + std::cos(a0) * radius, baseCenter.y,
                          baseCenter.z + std::sin(a0) * radius);
        const XMFLOAT3 b1(baseCenter.x + std::cos(a1) * radius, baseCenter.y,
                          baseCenter.z + std::sin(a1) * radius);

        const XMFLOAT3 edge0(b1.x - b0.x, b1.y - b0.y, b1.z - b0.z);
        const XMFLOAT3 edge1(apex.x - b0.x, apex.y - b0.y, apex.z - b0.z);
        XMFLOAT3 normal(edge0.y * edge1.z - edge0.z * edge1.y,
                        edge0.z * edge1.x - edge0.x * edge1.z,
                        edge0.x * edge1.y - edge0.y * edge1.x);
        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                       normal.z * normal.z);
        if (length > 1e-6f) {
            normal.x /= length;
            normal.y /= length;
            normal.z /= length;
        }
        const XMFLOAT3 radial((b0.x + b1.x) * 0.5f - baseCenter.x, 0.0f,
                              (b0.z + b1.z) * 0.5f - baseCenter.z);
        if (normal.x * radial.x + normal.z * radial.z < 0.0f) {
            normal.x = -normal.x;
            normal.y = -normal.y;
            normal.z = -normal.z;
        }

        AddTriangle(vertices, indices, b0, b1, apex, normal, color);
        AddTriangle(vertices, indices, baseCenter, b1, b0, XMFLOAT3(0, -1, 0), color);
    }
}

} // namespace

bool SceneRenderer::Initialize(ID3D11Device* device) {
    if (!device) {
        return false;
    }
    device_ = device;

    const std::wstring path = ResolveAssetPath(L"assets\\shaders\\scene.hlsl");
    if (path.empty()) {
        LogError("Scene shader not found.");
        return false;
    }

    std::string errors;
    auto vertexBlob = CompileShaderFile(path, "VSScene", "vs_5_0", errors);
    auto shadowBlob = CompileShaderFile(path, "VSShadow", "vs_5_0", errors);
    auto pixelBlob = CompileShaderFile(path, "PSScene", "ps_5_0", errors);
    if (!vertexBlob || !shadowBlob || !pixelBlob) {
        LogError("Scene shaders failed to compile.");
        return false;
    }

    if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                          vertexBlob->GetBufferSize(), nullptr,
                                          &vertexShader_)) ||
        FAILED(device->CreateVertexShader(shadowBlob->GetBufferPointer(),
                                          shadowBlob->GetBufferSize(), nullptr,
                                          &shadowVertexShader_)) ||
        FAILED(device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                         pixelBlob->GetBufferSize(), nullptr,
                                         &pixelShader_))) {
        LogError("Scene shader objects could not be created.");
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(layout, UINT(std::size(layout)),
                                         vertexBlob->GetBufferPointer(),
                                         vertexBlob->GetBufferSize(), &inputLayout_))) {
        LogError("Scene input layout creation failed.");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthState_))) {
        LogError("Scene depth state creation failed.");
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizer_))) {
        LogError("Scene rasterizer creation failed.");
        return false;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &blendState_))) {
        LogError("Scene blend state creation failed.");
        return false;
    }

    BuildMeshes();
    if (!CreateShadowResources(device)) {
        LogError("Scene shadow resources could not be created.");
        return false;
    }
    return true;
}

bool SceneRenderer::CreateShadowResources(ID3D11Device* device) {
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = 2048;
    textureDesc.Height = 2048;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&textureDesc, nullptr, &shadowTexture_))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateDepthStencilView(shadowTexture_.Get(), &dsvDesc,
                                              &shadowDSV_))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(shadowTexture_.Get(), &srvDesc,
                                                &shadowSRV_))) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDesc.MaxLOD = 0.0f;
    return SUCCEEDED(device->CreateSamplerState(&samplerDesc, &shadowSampler_));
}

void SceneRenderer::Shutdown() {
    shadowSampler_.Reset();
    shadowSRV_.Reset();
    shadowDSV_.Reset();
    shadowTexture_.Reset();
    meshes_.clear();
    blendState_.Reset();
    rasterizer_.Reset();
    depthState_.Reset();
    inputLayout_.Reset();
    pixelShader_.Reset();
    shadowVertexShader_.Reset();
    vertexShader_.Reset();
    device_ = nullptr;
}

bool SceneRenderer::CreateMesh(const std::vector<Vertex>& vertices,
                               const std::vector<uint32_t>& indices, Mesh& mesh) {
    if (!device_ || vertices.empty() || indices.empty()) {
        return false;
    }

    D3D11_BUFFER_DESC vertexDesc{};
    vertexDesc.ByteWidth = UINT(vertices.size() * sizeof(Vertex));
    vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{};
    vertexData.pSysMem = vertices.data();
    if (FAILED(device_->CreateBuffer(&vertexDesc, &vertexData, &mesh.vertexBuffer))) {
        return false;
    }

    D3D11_BUFFER_DESC indexDesc{};
    indexDesc.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
    indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{};
    indexData.pSysMem = indices.data();
    if (FAILED(device_->CreateBuffer(&indexDesc, &indexData, &mesh.indexBuffer))) {
        return false;
    }

    mesh.indexCount = UINT(indices.size());
    return EnsureInstanceCapacity(mesh, 64);
}

bool SceneRenderer::EnsureInstanceCapacity(Mesh& mesh, UINT capacity) {
    if (!device_) {
        return false;
    }
    if (capacity <= mesh.instanceCapacity) {
        return true;
    }

    capacity = std::max({capacity, 64u, mesh.instanceCapacity * 2u});

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = capacity * sizeof(Instance);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(Instance);

    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &buffer))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = capacity;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateShaderResourceView(buffer.Get(), &srvDesc, &srv))) {
        return false;
    }

    mesh.instanceBuffer = buffer;
    mesh.instanceSRV = srv;
    mesh.instanceCapacity = capacity;
    return true;
}

void SceneRenderer::BuildMeshes() {
    meshes_.resize(8);
    const XMFLOAT3 white{1.0f, 1.0f, 1.0f};
    const XMFLOAT3 brown{0.22f, 0.14f, 0.08f};

    // 0: building, unit box with its base at y = 0.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.5f, 0.0f),
               XMFLOAT3(0.5f, 0.5f, 0.5f), white);
        CreateMesh(vertices, indices, meshes_[0]);
    }

    // 1: car. Canonical footprint 2.0 x 1.6 x 5.0.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.45f, 0.0f),
               XMFLOAT3(0.95f, 0.25f, 2.15f), XMFLOAT3(0.55f, 0.12f, 0.10f));
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.82f, -0.15f),
               XMFLOAT3(0.78f, 0.28f, 1.15f), XMFLOAT3(0.10f, 0.13f, 0.17f));
        for (float sx : {-1.0f, 1.0f}) {
            for (float sz : {-1.0f, 1.0f}) {
                AddBox(vertices, indices,
                       XMFLOAT3(sx * 0.90f, 0.22f, sz * 1.45f),
                       XMFLOAT3(0.16f, 0.22f, 0.30f), XMFLOAT3(0.05f, 0.05f, 0.05f));
            }
        }
        CreateMesh(vertices, indices, meshes_[1]);
    }

    // 2: van.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.65f, 0.0f),
               XMFLOAT3(0.95f, 0.55f, 2.40f), XMFLOAT3(0.85f, 0.85f, 0.82f));
        AddBox(vertices, indices, XMFLOAT3(0.0f, 1.15f, 1.55f),
               XMFLOAT3(0.85f, 0.22f, 0.55f), XMFLOAT3(0.10f, 0.13f, 0.17f));
        for (float sx : {-1.0f, 1.0f}) {
            for (float sz : {-1.0f, 1.0f}) {
                AddBox(vertices, indices,
                       XMFLOAT3(sx * 0.90f, 0.22f, sz * 1.55f),
                       XMFLOAT3(0.16f, 0.22f, 0.32f), XMFLOAT3(0.05f, 0.05f, 0.05f));
            }
        }
        CreateMesh(vertices, indices, meshes_[2]);
    }

    // 3: truck.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.75f, 1.55f),
               XMFLOAT3(0.90f, 0.55f, 0.70f), XMFLOAT3(0.20f, 0.30f, 0.55f));
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.95f, -0.80f),
               XMFLOAT3(0.95f, 0.70f, 1.70f), XMFLOAT3(0.55f, 0.50f, 0.42f));
        for (float sx : {-1.0f, 1.0f}) {
            for (float sz : {-1.0f, 1.0f}) {
                AddBox(vertices, indices,
                       XMFLOAT3(sx * 0.92f, 0.24f, sz * 1.70f),
                       XMFLOAT3(0.17f, 0.24f, 0.34f), XMFLOAT3(0.05f, 0.05f, 0.05f));
            }
        }
        CreateMesh(vertices, indices, meshes_[3]);
    }

    // 4-6: tree variants. Canonical height 1, canopy radius 0.5.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.25f, 0.0f),
               XMFLOAT3(0.04f, 0.25f, 0.04f), brown);
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.32f, 0.0f), 0.50f, 0.48f,
                XMFLOAT3(0.12f, 0.30f, 0.10f));
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.52f, 0.0f), 0.38f, 0.36f,
                XMFLOAT3(0.16f, 0.36f, 0.12f));
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.70f, 0.0f), 0.25f, 0.30f,
                XMFLOAT3(0.20f, 0.40f, 0.14f));
        CreateMesh(vertices, indices, meshes_[4]);
    }
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.30f, 0.0f),
               XMFLOAT3(0.035f, 0.30f, 0.035f), brown);
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.35f, 0.0f), 0.40f, 0.40f,
                XMFLOAT3(0.10f, 0.28f, 0.09f));
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.55f, 0.0f), 0.30f, 0.35f,
                XMFLOAT3(0.15f, 0.34f, 0.11f));
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.72f, 0.0f), 0.20f, 0.28f,
                XMFLOAT3(0.19f, 0.38f, 0.13f));
        CreateMesh(vertices, indices, meshes_[5]);
    }
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.20f, 0.0f),
               XMFLOAT3(0.05f, 0.20f, 0.05f), brown);
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.25f, 0.0f), 0.50f, 0.45f,
                XMFLOAT3(0.14f, 0.32f, 0.11f));
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.45f, 0.0f), 0.42f, 0.35f,
                XMFLOAT3(0.18f, 0.38f, 0.13f));
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.62f, 0.0f), 0.30f, 0.30f,
                XMFLOAT3(0.22f, 0.42f, 0.15f));
        CreateMesh(vertices, indices, meshes_[6]);
    }

    // 7: unit box centred at the origin, used for building chunks.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBox(vertices, indices, XMFLOAT3(0.0f, 0.0f, 0.0f),
               XMFLOAT3(0.5f, 0.5f, 0.5f), white);
        CreateMesh(vertices, indices, meshes_[7]);
    }
}

void SceneRenderer::RenderShadow(ID3D11DeviceContext* context,
                                 const scene::Scene& scene, const ViewInfo& view) {
    if (!device_ || !context) {
        return;
    }
    // Always clear so the ground samples a fully lit map when the scene is off.
    context->ClearDepthStencilView(shadowDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (!scene.Settings().enabled || scene.Empty()) {
        return;
    }

    const auto& bodies = scene.Physics().Bodies();
    FrustumPlanes frustum;
    frustum.FromViewProj(view.viewProj);
    auto visible = [&frustum](const XMFLOAT3& center, float radius) {
        return frustum.ContainsSphere(center, radius);
    };

    for (Mesh& mesh : meshes_) {
        mesh.instances.clear();
    }

    for (const scene::SceneObject& object : scene.Objects()) {
        if (object.collapsed) {
            continue;
        }

        const XMFLOAT3 objectCenter(object.position.x, object.size.y * 0.5f,
                                    object.position.z);
        const float objectRadius =
            std::max({object.size.x, object.size.y, object.size.z}) * 0.75f + 1.0f;
        // Keep casters slightly outside the view so their shadows can still
        // reach into the visible area.
        if (!visible(objectCenter, objectRadius + 20.0f)) {
            continue;
        }

        int meshIndex = -1;
        XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
        XMFLOAT4 color = object.color;
        float sway = 0.0f;
        float type = 0.0f;

        switch (object.type) {
        case scene::SceneObjectType::Building:
            meshIndex = 0;
            scale = object.size;
            type = 0.0f;
            break;
        case scene::SceneObjectType::Vehicle:
            meshIndex = 1 + (object.meshVariant % 3);
            scale = XMFLOAT3(object.size.x / 2.0f, object.size.y / 1.6f,
                             object.size.z / 5.0f);
            type = 1.0f;
            break;
        case scene::SceneObjectType::Tree:
            meshIndex = 4 + (object.meshVariant % 3);
            scale = XMFLOAT3(object.size.x * 2.0f, object.size.y,
                             object.size.x * 2.0f);
            sway = 0.05f;
            type = 2.0f;
            break;
        default:
            continue;
        }

        if (meshIndex < 0 || meshIndex >= int(meshes_.size())) {
            continue;
        }

        Instance instance{};
        XMMATRIX world;
        if (object.bodyIndex >= 0 && object.bodyIndex < int(bodies.size())) {
            const scene::RigidBody& body = bodies[object.bodyIndex];
            world = XMMatrixTranslation(0.0f, -0.5f, 0.0f) *
                    XMMatrixScaling(scale.x, scale.y, scale.z) *
                    XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation)) *
                    XMMatrixTranslation(body.position.x, body.position.y,
                                        body.position.z);
        } else {
            world = XMMatrixScaling(scale.x, scale.y, scale.z) *
                    XMMatrixRotationY(XMConvertToRadians(object.yaw)) *
                    XMMatrixTranslation(object.position.x, 0.0f, object.position.z);
        }
        XMStoreFloat4x4(&instance.transform, world);
        instance.color = color;
        instance.params = XMFLOAT4(object.damageAmount, sway, object.swayPhase, type);
        instance.state = XMFLOAT4(object.burning ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        meshes_[meshIndex].instances.push_back(instance);
    }

    // Building chunks: unit boxes driven by the rigid-body transforms, plus the
    // chunks that are still supported by the structural graph.
    for (const scene::SceneObject& object : scene.Objects()) {
        if (!object.collapsed) {
            continue;
        }
        for (const scene::SceneChunk& chunk : object.chunks) {
            XMFLOAT3 chunkCenter = chunk.center;
            if (chunk.bodyIndex >= 0 && chunk.bodyIndex < int(bodies.size())) {
                chunkCenter = bodies[chunk.bodyIndex].position;
            }
            const float chunkRadius =
                std::sqrt(chunk.half.x * chunk.half.x + chunk.half.y * chunk.half.y +
                          chunk.half.z * chunk.half.z) *
                1.3f;
            if (!visible(chunkCenter, chunkRadius)) {
                continue;
            }
            // Level of detail: drop chunks that cover less than a pixel.
            const float chunkDistance = XMVectorGetX(
                XMVector3Length(XMLoadFloat3(&chunkCenter) - XMLoadFloat3(&view.eye)));
            if (chunkRadius * view.projScale / std::max(chunkDistance, 0.01f) < 1.0f) {
                continue;
            }

            Instance instance{};
            XMMATRIX world;
            if (chunk.bodyIndex >= 0 && chunk.bodyIndex < int(bodies.size())) {
                const scene::RigidBody& body = bodies[chunk.bodyIndex];
                world = XMMatrixScaling(chunk.half.x * 2.0f, chunk.half.y * 2.0f,
                                        chunk.half.z * 2.0f) *
                        XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation)) *
                        XMMatrixTranslation(body.position.x, body.position.y,
                                            body.position.z);
            } else {
                world = XMMatrixScaling(chunk.half.x * 2.0f, chunk.half.y * 2.0f,
                                        chunk.half.z * 2.0f) *
                        XMMatrixTranslation(chunk.center.x, chunk.center.y,
                                            chunk.center.z);
            }
            XMStoreFloat4x4(&instance.transform, world);
            instance.color = chunk.color;
            instance.params = XMFLOAT4(1.0f, 0.0f, 0.0f, 3.0f);
            instance.state = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
            meshes_[7].instances.push_back(instance);
        }
    }

    // Upload all instance buffers once; both the shadow and the main pass use them.
    for (Mesh& mesh : meshes_) {
        if (mesh.instances.empty() || !mesh.vertexBuffer || !mesh.indexBuffer) {
            continue;
        }
        if (!EnsureInstanceCapacity(mesh, UINT(mesh.instances.size()))) {
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(mesh.instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                &mapped))) {
            continue;
        }
        std::memcpy(mapped.pData, mesh.instances.data(),
                    mesh.instances.size() * sizeof(Instance));
        context->Unmap(mesh.instanceBuffer.Get(), 0);
    }

    // --- Shadow pass -------------------------------------------------------
    context->OMSetRenderTargets(0, nullptr, shadowDSV_.Get());
    context->ClearDepthStencilView(shadowDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    context->OMSetDepthStencilState(depthState_.Get(), 0);
    context->OMSetBlendState(blendState_.Get(), nullptr, 0xffffffff);
    context->RSSetState(rasterizer_.Get());
    context->IASetInputLayout(inputLayout_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(shadowVertexShader_.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);

    D3D11_VIEWPORT shadowViewport{0.0f, 0.0f, 2048.0f, 2048.0f, 0.0f, 1.0f};
    context->RSSetViewports(1, &shadowViewport);

    for (Mesh& mesh : meshes_) {
        if (mesh.instances.empty() || !mesh.instanceBuffer) {
            continue;
        }
        ID3D11Buffer* vertexBuffer = mesh.vertexBuffer.Get();
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        ID3D11ShaderResourceView* srv = mesh.instanceSRV.Get();
        context->VSSetShaderResources(0, 1, &srv);
        context->DrawIndexedInstanced(mesh.indexCount, UINT(mesh.instances.size()), 0, 0,
                                      0);
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->VSSetShaderResources(0, 1, &nullSrv);

}

void SceneRenderer::RenderScene(ID3D11DeviceContext* context, const scene::Scene& scene,
                                ID3D11RenderTargetView* hdrRTV,
                                ID3D11DepthStencilView* depthDSV, int width,
                                int height) {
    if (!device_ || !context || !hdrRTV || !depthDSV) {
        return;
    }
    if (!scene.Settings().enabled || scene.Empty()) {
        return;
    }

    context->OMSetRenderTargets(1, &hdrRTV, depthDSV);
    context->OMSetDepthStencilState(depthState_.Get(), 0);
    context->OMSetBlendState(blendState_.Get(), nullptr, 0xffffffff);
    context->RSSetState(rasterizer_.Get());
    context->IASetInputLayout(inputLayout_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader_.Get(), nullptr, 0);

    D3D11_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);

    ID3D11ShaderResourceView* shadowView = shadowSRV_.Get();
    context->PSSetShaderResources(2, 1, &shadowView);
    ID3D11SamplerState* shadowSampler = shadowSampler_.Get();
    context->PSSetSamplers(2, 1, &shadowSampler);

    for (Mesh& mesh : meshes_) {
        if (mesh.instances.empty() || !mesh.instanceBuffer) {
            continue;
        }
        ID3D11Buffer* vertexBuffer = mesh.vertexBuffer.Get();
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(mesh.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        ID3D11ShaderResourceView* srv = mesh.instanceSRV.Get();
        context->VSSetShaderResources(0, 1, &srv);
        context->DrawIndexedInstanced(mesh.indexCount, UINT(mesh.instances.size()), 0, 0,
                                      0);
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11ShaderResourceView* nullShadow = nullptr;
    context->PSSetShaderResources(2, 1, &nullShadow);
    ID3D11SamplerState* nullSampler = nullptr;
    context->PSSetSamplers(2, 1, &nullSampler);
    context->VSSetShaderResources(0, 1, &nullSrv);
    context->IASetInputLayout(nullptr);
}

} // namespace ex::gfx
