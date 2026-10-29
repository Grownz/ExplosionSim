#include "gfx/SceneRenderer.h"

#include "core/Log.h"
#include "gfx/Frustum.h"
#include "gfx/Shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

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

XMFLOAT3 Normalize3(const XMFLOAT3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-6f) {
        return XMFLOAT3(0.0f, 1.0f, 0.0f);
    }
    return XMFLOAT3(v.x / length, v.y / length, v.z / length);
}

// Emits a quad with the winding flipped if needed so the geometric normal points
// along `normal`.
void AddQuadOriented(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                     const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                     const XMFLOAT3& d, const XMFLOAT3& normal, const XMFLOAT3& color) {
    auto emit = [&](const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2) {
        const XMFLOAT3 e1(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
        const XMFLOAT3 e2(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
        const XMFLOAT3 geometric(e1.y * e2.z - e1.z * e2.y,
                                 e1.z * e2.x - e1.x * e2.z,
                                 e1.x * e2.y - e1.y * e2.x);
        if (geometric.x * normal.x + geometric.y * normal.y + geometric.z * normal.z <
            0.0f) {
            AddTriangle(vertices, indices, p0, p2, p1, normal, color);
        } else {
            AddTriangle(vertices, indices, p0, p1, p2, normal, color);
        }
    };
    emit(a, b, c);
    emit(a, c, d);
}

// Emits a triangle with the winding flipped if needed so the geometric normal
// points along `normal`.
void AddTriangleOriented(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                         const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                         const XMFLOAT3& normal, const XMFLOAT3& color) {
    const XMFLOAT3 e1(b.x - a.x, b.y - a.y, b.z - a.z);
    const XMFLOAT3 e2(c.x - a.x, c.y - a.y, c.z - a.z);
    const XMFLOAT3 geometric(e1.y * e2.z - e1.z * e2.y,
                             e1.z * e2.x - e1.x * e2.z,
                             e1.x * e2.y - e1.y * e2.x);
    if (geometric.x * normal.x + geometric.y * normal.y + geometric.z * normal.z < 0.0f) {
        AddTriangle(vertices, indices, a, c, b, normal, color);
    } else {
        AddTriangle(vertices, indices, a, b, c, normal, color);
    }
}

// Box with chamfered edges and corners. The physics body stays a box; the bevel
// removes the hard right angles from the rubble silhouette.
void AddBeveledBox(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                   const XMFLOAT3& half, float bevel, const XMFLOAT3& color) {
    const float b = std::min({bevel, half.x * 0.35f, half.y * 0.35f, half.z * 0.35f});
    const float hx = half.x;
    const float hy = half.y;
    const float hz = half.z;

    // Vertex on the `axis` face at the corner (sx, sy, sz); the two tangent
    // coordinates are inset by the bevel.
    auto vertex = [&](int axis, float sx, float sy, float sz) {
        return XMFLOAT3(sx * (axis == 0 ? hx : hx - b),
                        sy * (axis == 1 ? hy : hy - b),
                        sz * (axis == 2 ? hz : hz - b));
    };

    struct FaceCorners {
        int axis;
        float signs[4][3];
        XMFLOAT3 normal;
    };
    const FaceCorners faces[6] = {
        {0, {{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}}, {1, 0, 0}},
        {0, {{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}}, {-1, 0, 0}},
        {1, {{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}, {0, 1, 0}},
        {1, {{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}}, {0, -1, 0}},
        {2, {{-1, -1, 1}, {-1, 1, 1}, {1, 1, 1}, {1, -1, 1}}, {0, 0, 1}},
        {2, {{1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, -1}}, {0, 0, -1}},
    };

    for (const FaceCorners& face : faces) {
        XMFLOAT3 corners[4];
        for (int i = 0; i < 4; ++i) {
            corners[i] = vertex(face.axis, face.signs[i][0], face.signs[i][1],
                                face.signs[i][2]);
        }
        AddQuadOriented(vertices, indices, corners[0], corners[1], corners[2],
                        corners[3], face.normal, color);
    }

    // Edge chamfers: 12 quads.
    for (int s0 = -1; s0 <= 1; s0 += 2) {
        for (int s1 = -1; s1 <= 1; s1 += 2) {
            // Along Z: X face and Y face.
            {
                const XMFLOAT3 a = vertex(0, float(s0), float(s1), -1.0f);
                const XMFLOAT3 b2 = vertex(0, float(s0), float(s1), 1.0f);
                const XMFLOAT3 c = vertex(1, float(s0), float(s1), 1.0f);
                const XMFLOAT3 d = vertex(1, float(s0), float(s1), -1.0f);
                AddQuadOriented(vertices, indices, a, b2, c, d,
                                Normalize3(XMFLOAT3(float(s0), float(s1), 0.0f)), color);
            }
            // Along Y: X face and Z face.
            {
                const XMFLOAT3 a = vertex(0, float(s0), -1.0f, float(s1));
                const XMFLOAT3 b2 = vertex(0, float(s0), 1.0f, float(s1));
                const XMFLOAT3 c = vertex(2, float(s0), 1.0f, float(s1));
                const XMFLOAT3 d = vertex(2, float(s0), -1.0f, float(s1));
                AddQuadOriented(vertices, indices, a, b2, c, d,
                                Normalize3(XMFLOAT3(float(s0), 0.0f, float(s1))), color);
            }
            // Along X: Y face and Z face.
            {
                const XMFLOAT3 a = vertex(1, -1.0f, float(s0), float(s1));
                const XMFLOAT3 b2 = vertex(1, 1.0f, float(s0), float(s1));
                const XMFLOAT3 c = vertex(2, 1.0f, float(s0), float(s1));
                const XMFLOAT3 d = vertex(2, -1.0f, float(s0), float(s1));
                AddQuadOriented(vertices, indices, a, b2, c, d,
                                Normalize3(XMFLOAT3(0.0f, float(s0), float(s1))), color);
            }
        }
    }

    // Corner triangles: 8.
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const XMFLOAT3 a = vertex(0, float(sx), float(sy), float(sz));
                const XMFLOAT3 b2 = vertex(1, float(sx), float(sy), float(sz));
                const XMFLOAT3 c = vertex(2, float(sx), float(sy), float(sz));
                const XMFLOAT3 normal =
                    Normalize3(XMFLOAT3(float(sx), float(sy), float(sz)));
                AddTriangleOriented(vertices, indices, a, b2, c, normal, color);
            }
        }
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

bool SceneRenderer::Initialize(ID3D11Device* device, rhi::Device* rhiDevice) {
    if (!device || !rhiDevice) {
        return false;
    }
    device_ = device;
    rhiDevice_ = rhiDevice;

    const std::wstring path = ResolveAssetPath(L"assets\\shaders\\scene.hlsl");
    if (path.empty()) {
        LogError("Scene shader not found.");
        return false;
    }

    std::string errors;
    auto vertexBlob = CompileShaderFile(path, "VSScene", "vs_5_0", errors);
    auto shadowBlob = CompileShaderFile(path, "VSShadow", "vs_5_0", errors);
    auto chunkBlob = CompileShaderFile(path, "VSChunk", "vs_5_0", errors);
    auto chunkShadowBlob =
        CompileShaderFile(path, "VSShadowChunk", "vs_5_0", errors);
    auto pixelBlob = CompileShaderFile(path, "PSScene", "ps_5_0", errors);
    if (!vertexBlob || !shadowBlob || !chunkBlob || !chunkShadowBlob ||
        !pixelBlob) {
        LogError("Scene shaders failed to compile.");
        return false;
    }

    auto makeShader = [this](rhi::ShaderStage stage, const ComPtr<ID3DBlob>& blob) {
        rhi::ShaderDesc desc{};
        desc.stage = stage;
        desc.data = blob->GetBufferPointer();
        desc.size = blob->GetBufferSize();
        return rhiDevice_->CreateShader(desc);
    };

    auto vertexShader = makeShader(rhi::ShaderStage::Vertex, vertexBlob);
    auto shadowShader = makeShader(rhi::ShaderStage::Vertex, shadowBlob);
    auto chunkShader = makeShader(rhi::ShaderStage::Vertex, chunkBlob);
    auto chunkShadowShader = makeShader(rhi::ShaderStage::Vertex, chunkShadowBlob);
    auto pixelShader = makeShader(rhi::ShaderStage::Pixel, pixelBlob);
    if (!vertexShader || !shadowShader || !chunkShader || !chunkShadowShader ||
        !pixelShader) {
        LogError("Scene shader objects could not be created.");
        return false;
    }

    const rhi::VertexAttribute sceneAttributes[] = {
        {"POSITION", 0, rhi::VertexFormat::Float3, 0, 0},
        {"NORMAL", 0, rhi::VertexFormat::Float3, 12, 0},
        {"COLOR", 0, rhi::VertexFormat::Float3, 24, 0},
    };
    const rhi::VertexAttribute chunkAttributes[] = {
        {"POSITION", 0, rhi::VertexFormat::Float3, 0, 0},
        {"NORMAL", 0, rhi::VertexFormat::Float3, 12, 0},
        {"CHUNKID", 0, rhi::VertexFormat::UInt, 24, 0},
    };

    auto makePipeline = [this](rhi::Shader* vs, rhi::Shader* ps,
                               const rhi::VertexAttribute* attributes,
                               uint32_t attributeCount, const ComPtr<ID3DBlob>& bytecode) {
        rhi::PipelineDesc desc{};
        desc.vertexShader = vs;
        desc.pixelShader = ps;
        desc.vertexAttributes = attributes;
        desc.vertexAttributeCount = attributeCount;
        desc.vertexShaderBytecode = bytecode->GetBufferPointer();
        desc.vertexShaderBytecodeSize = bytecode->GetBufferSize();
        return rhiDevice_->CreatePipeline(desc);
    };

    sceneShadowPipeline_ =
        makePipeline(shadowShader.get(), nullptr, sceneAttributes,
                     uint32_t(std::size(sceneAttributes)), shadowBlob);
    chunkShadowPipeline_ =
        makePipeline(chunkShadowShader.get(), nullptr, chunkAttributes,
                     uint32_t(std::size(chunkAttributes)), chunkShadowBlob);
    scenePipeline_ = makePipeline(vertexShader.get(), pixelShader.get(),
                                  sceneAttributes, uint32_t(std::size(sceneAttributes)),
                                  vertexBlob);
    chunkPipeline_ = makePipeline(chunkShader.get(), pixelShader.get(),
                                  chunkAttributes, uint32_t(std::size(chunkAttributes)),
                                  chunkBlob);
    if (!sceneShadowPipeline_ || !chunkShadowPipeline_ || !scenePipeline_ ||
        !chunkPipeline_) {
        LogError("Scene pipelines could not be created.");
        return false;
    }

    BuildMeshes();
    if (!CreateShadowResources(device)) {
        LogError("Scene shadow resources could not be created.");
        return false;
    }
    if (!CreateSurfaceDetailTexture(device)) {
        LogError("Scene surface detail texture could not be created.");
        return false;
    }
    return true;
}

bool SceneRenderer::CreateSurfaceDetailTexture(ID3D11Device* device) {
    constexpr int kSize = 256;

    auto hash = [](int x, int y, int seed) {
        uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u +
                     uint32_t(seed) * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return float(h & 0x00ffffffu) / 16777215.0f;
    };
    auto wrap = [](int v, int period) {
        v %= period;
        return v < 0 ? v + period : v;
    };
    auto noise = [&hash, &wrap](float x, float y, int period) {
        const int x0 = int(std::floor(x));
        const int y0 = int(std::floor(y));
        const float fx = x - float(x0);
        const float fy = y - float(y0);
        const float sx = fx * fx * (3.0f - 2.0f * fx);
        const float sy = fy * fy * (3.0f - 2.0f * fy);
        const int xa = wrap(x0, period);
        const int ya = wrap(y0, period);
        const int xb = wrap(x0 + 1, period);
        const int yb = wrap(y0 + 1, period);
        const float n00 = hash(xa, ya, period);
        const float n10 = hash(xb, ya, period);
        const float n01 = hash(xa, yb, period);
        const float n11 = hash(xb, yb, period);
        const float nx0 = n00 + (n10 - n00) * sx;
        const float nx1 = n01 + (n11 - n01) * sx;
        return nx0 + (nx1 - nx0) * sy;
    };
    auto fbm = [&noise](float x, float y, int basePeriod, int seedOffset) {
        float value = 0.0f;
        float amplitude = 0.55f;
        float scale = 1.0f;
        int period = basePeriod;
        for (int octave = 0; octave < 5; ++octave) {
            value += amplitude * noise(x * scale + float(seedOffset),
                                       y * scale + float(seedOffset), period);
            amplitude *= 0.5f;
            scale *= 2.0f;
            period *= 2;
        }
        return std::clamp(value, 0.0f, 1.0f);
    };

    std::vector<uint8_t> base(size_t(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float u = float(x) / float(kSize);
            const float v = float(y) / float(kSize);
            const float grain = fbm(u * 8.0f, v * 8.0f, 8, 0);
            const float grime = fbm(u * 3.0f, v * 3.0f, 3, 17);
            const float speckle = hash(x, y, 91);
            const size_t index = (size_t(y) * kSize + size_t(x)) * 4;
            base[index + 0] = uint8_t(std::clamp(grain, 0.0f, 1.0f) * 255.0f);
            base[index + 1] = uint8_t(std::clamp(grime, 0.0f, 1.0f) * 255.0f);
            base[index + 2] = uint8_t(speckle * 255.0f);
            base[index + 3] = 255;
        }
    }

    // Build the complete mip chain on the CPU so the texture can stay immutable.
    std::vector<std::vector<uint8_t>> levels;
    levels.push_back(std::move(base));
    int width = kSize;
    while (width > 1) {
        const int nextWidth = width / 2;
        std::vector<uint8_t> next(size_t(nextWidth) * nextWidth * 4);
        const std::vector<uint8_t>& src = levels.back();
        for (int y = 0; y < nextWidth; ++y) {
            for (int x = 0; x < nextWidth; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const size_t index =
                                (size_t(y * 2 + dy) * width + size_t(x * 2 + dx)) * 4 +
                                size_t(channel);
                            sum += src[index];
                        }
                    }
                    const size_t index =
                        (size_t(y) * nextWidth + size_t(x)) * 4 + size_t(channel);
                    next[index] = uint8_t(sum / 4);
                }
            }
        }
        levels.push_back(std::move(next));
        width = nextWidth;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kSize;
    desc.Height = kSize;
    desc.MipLevels = UINT(levels.size());
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> subresources(levels.size());
    int levelWidth = kSize;
    for (size_t level = 0; level < levels.size(); ++level) {
        subresources[level].pSysMem = levels[level].data();
        subresources[level].SysMemPitch = UINT(levelWidth * 4);
        subresources[level].SysMemSlicePitch = 0;
        levelWidth = std::max(1, levelWidth / 2);
    }
    if (FAILED(device->CreateTexture2D(&desc, subresources.data(),
                                       &surfaceDetailTexture_))) {
        return false;
    }
    return SUCCEEDED(device->CreateShaderResourceView(
        surfaceDetailTexture_.Get(), nullptr, &surfaceDetailSRV_));
}

bool SceneRenderer::CreateShadowResources(ID3D11Device* device) {
    for (int cascade = 0; cascade < kShadowCascades; ++cascade) {
        const int size = ShadowSize(cascade);
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = UINT(size);
        textureDesc.Height = UINT(size);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&textureDesc, nullptr,
                                           &shadowTexture_[cascade]))) {
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateDepthStencilView(shadowTexture_[cascade].Get(),
                                                  &dsvDesc, &shadowDSV_[cascade]))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(shadowTexture_[cascade].Get(),
                                                    &srvDesc, &shadowSRV_[cascade]))) {
            return false;
        }
    }

    // Cached static-caster map: world space, independent of the camera.
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = UINT(kStaticShadowSize);
        textureDesc.Height = UINT(kStaticShadowSize);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags =
            D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&textureDesc, nullptr,
                                           &staticShadowTexture_))) {
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateDepthStencilView(staticShadowTexture_.Get(),
                                                  &dsvDesc,
                                                  &staticShadowDSV_))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(staticShadowTexture_.Get(),
                                                    &srvDesc,
                                                    &staticShadowSRV_))) {
            return false;
        }
    }

    rhi::BufferDesc bufferDesc{};
    bufferDesc.size = sizeof(XMFLOAT4X4);
    bufferDesc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::Dynamic;
    shadowCascadeBuffer_ = rhiDevice_->CreateBuffer(bufferDesc);
    if (!shadowCascadeBuffer_) {
        return false;
    }

    rhi::SamplerDesc samplerDesc{};
    samplerDesc.comparison = true;
    samplerDesc.clamp = false;
    samplerDesc.linear = true;
    samplerDesc.border[0] = 1.0f;
    samplerDesc.border[1] = 1.0f;
    samplerDesc.border[2] = 1.0f;
    samplerDesc.border[3] = 1.0f;
    shadowSampler_ = rhiDevice_->CreateSampler(samplerDesc);
    return shadowSampler_ != nullptr;
}

void SceneRenderer::Shutdown() {
    shadowCascadeBuffer_.reset();
    shadowSampler_.reset();
    surfaceDetailSRV_.Reset();
    surfaceDetailTexture_.Reset();
    for (int cascade = 0; cascade < kShadowCascades; ++cascade) {
        shadowSRV_[cascade].Reset();
        shadowDSV_[cascade].Reset();
        shadowTexture_[cascade].Reset();
    }
    staticInstanceBuffer_.reset();
    staticShadowSRV_.Reset();
    staticShadowDSV_.Reset();
    staticShadowTexture_.Reset();
    staticInstanceCapacity_ = 0;
    staticShadowKey_ = 0;
    staticShadowValid_ = false;
    meshes_.clear();
    chunkInstances_.clear();
    chunkDraws_.clear();
    chunkInstanceCount_ = 0;
    chunkInstanceCapacity_ = 0;
    chunkGeometryKey_ = 0;
    chunkGeometryValid_ = false;
    chunkInstanceBuffer_.reset();
    chunkIndexBuffer_.reset();
    chunkVertexBuffer_.reset();
    chunkPipeline_.reset();
    scenePipeline_.reset();
    chunkShadowPipeline_.reset();
    sceneShadowPipeline_.reset();
    device_ = nullptr;
    rhiDevice_ = nullptr;
}

bool SceneRenderer::CreateMesh(const std::vector<Vertex>& vertices,
                               const std::vector<uint32_t>& indices, Mesh& mesh) {
    if (!rhiDevice_ || vertices.empty() || indices.empty()) {
        return false;
    }

    rhi::BufferDesc vertexDesc{};
    vertexDesc.size = vertices.size() * sizeof(Vertex);
    vertexDesc.usage = rhi::BufferUsage::Vertex;
    vertexDesc.initialData = vertices.data();
    mesh.vertexBuffer = rhiDevice_->CreateBuffer(vertexDesc);
    if (!mesh.vertexBuffer) {
        return false;
    }

    rhi::BufferDesc indexDesc{};
    indexDesc.size = indices.size() * sizeof(uint32_t);
    indexDesc.usage = rhi::BufferUsage::Index;
    indexDesc.initialData = indices.data();
    mesh.indexBuffer = rhiDevice_->CreateBuffer(indexDesc);
    if (!mesh.indexBuffer) {
        return false;
    }

    mesh.indexCount = UINT(indices.size());
    return EnsureInstanceCapacity(mesh, 64);
}

bool SceneRenderer::EnsureInstanceCapacity(Mesh& mesh, UINT capacity) {
    if (!rhiDevice_) {
        return false;
    }
    if (capacity <= mesh.instanceCapacity) {
        return true;
    }

    capacity = std::max({capacity, 64u, mesh.instanceCapacity * 2u});

    rhi::BufferDesc desc{};
    desc.size = uint64_t(capacity) * sizeof(Instance);
    desc.usage = rhi::BufferUsage::ShaderRead | rhi::BufferUsage::Dynamic;
    desc.structureStride = sizeof(Instance);
    std::unique_ptr<rhi::Buffer> buffer = rhiDevice_->CreateBuffer(desc);
    if (!buffer) {
        return false;
    }

    mesh.instanceBuffer = std::move(buffer);
    mesh.instanceCapacity = capacity;
    return true;
}

bool SceneRenderer::LoadVehicleMesh(const wchar_t* asset, Mesh& mesh) {
    static_assert(sizeof(Vertex) == 36, "ESV vertex layout");
    if (!device_ || !asset) {
        return false;
    }

    const std::wstring relative =
        std::wstring(L"assets\\models\\vehicles\\") + asset;
    const std::wstring path = ResolveAssetPath(relative.c_str());
    if (path.empty()) {
        return false;
    }

    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        return false;
    }

    char magic[4]{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    float sizeZ = 0.0f;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
    file.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
    file.read(reinterpret_cast<char*>(&sizeX), sizeof(sizeX));
    file.read(reinterpret_cast<char*>(&sizeY), sizeof(sizeY));
    file.read(reinterpret_cast<char*>(&sizeZ), sizeof(sizeZ));
    if (!file || std::memcmp(magic, "ESV1", 4) != 0 || vertexCount == 0 ||
        indexCount == 0 || vertexCount > 200000 || indexCount > 600000) {
        LogWarn("Invalid vehicle mesh: %ls", path.c_str());
        return false;
    }

    std::vector<Vertex> vertices(vertexCount);
    std::vector<uint32_t> indices(indexCount);
    file.read(reinterpret_cast<char*>(vertices.data()),
              std::streamsize(sizeof(Vertex) * vertexCount));
    file.read(reinterpret_cast<char*>(indices.data()),
              std::streamsize(sizeof(uint32_t) * indexCount));
    if (!file) {
        LogWarn("Truncated vehicle mesh: %ls", path.c_str());
        return false;
    }

    mesh.modelSize = XMFLOAT3(sizeX, sizeY, sizeZ);
    return CreateMesh(vertices, indices, mesh);
}

void SceneRenderer::BuildMeshes() {
    meshes_.resize(kMeshCount);
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

    // 1..kVehicleModelCount: imported low-poly vehicles. A missing asset
    // falls back to a simple box so the scene still renders.
    for (int i = 0; i < scene::kVehicleModelCount; ++i) {
        Mesh& mesh = meshes_[kVehicleMeshFirst + i];
        if (!LoadVehicleMesh(scene::kVehicleModels[i].asset, mesh)) {
            LogWarn("Vehicle model missing; using a placeholder: %ls",
                    scene::kVehicleModels[i].asset);
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            AddBox(vertices, indices, XMFLOAT3(0.0f, 0.65f, 0.0f),
                   XMFLOAT3(0.75f, 0.65f, 1.40f), white);
            CreateMesh(vertices, indices, mesh);
            mesh.modelSize = XMFLOAT3(1.5f, 1.3f, 2.8f);
        }
    }

    // kTreeMeshFirst..+2: tree variants. Canonical height 1, canopy radius 0.5.
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
        CreateMesh(vertices, indices, meshes_[kTreeMeshFirst]);
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
        CreateMesh(vertices, indices, meshes_[kTreeMeshFirst + 1]);
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
        CreateMesh(vertices, indices, meshes_[kTreeMeshFirst + 2]);
    }

    // 7: chamfered unit box, used for building chunks.
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddBeveledBox(vertices, indices, XMFLOAT3(0.5f, 0.5f, 0.5f), 0.10f, white);
        CreateMesh(vertices, indices, meshes_[kChunkMesh]);
    }

    // 8: low-detail tree used beyond the near distance (one cone, no trunk).
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        AddCone(vertices, indices, XMFLOAT3(0.0f, 0.25f, 0.0f), 0.48f, 0.75f,
                XMFLOAT3(0.14f, 0.33f, 0.11f));
        CreateMesh(vertices, indices, meshes_[kFarTreeMesh]);
    }
}

bool SceneRenderer::BuildChunkGeometry(const scene::Scene& scene) {
    if (!device_) {
        return false;
    }
    const bool enabled = scene.Settings().voronoiChunks;
    const uint64_t key =
        scene.GenerationHash() ^ (enabled ? 0x9E3779B97F4A7C15ull : 0x0ull);
    if (chunkGeometryValid_ && key == chunkGeometryKey_) {
        return true;
    }
    chunkGeometryValid_ = true;
    chunkGeometryKey_ = key;
    chunkDraws_.clear();
    chunkInstanceCount_ = 0;
    chunkInstances_.clear();
    chunkVertexBuffer_.reset();
    chunkIndexBuffer_.reset();
    if (!enabled || scene.Empty()) {
        return true;
    }

    std::vector<ChunkVertex> vertices;
    std::vector<uint32_t> indices;
    const std::vector<scene::SceneObject>& objects = scene.Objects();
    for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const scene::SceneObject& object = objects[objectIndex];
        if (object.chunks.empty() || object.chunks.front().hullPositions.empty()) {
            continue;
        }
        ChunkDraw draw;
        draw.indexOffset = UINT(indices.size());
        draw.objectIndex = int(objectIndex);
        for (const scene::SceneChunk& chunk : object.chunks) {
            if (chunk.hullPositions.empty() ||
                chunk.hullNormals.size() != chunk.hullPositions.size()) {
                continue;
            }
            const uint32_t chunkId = chunkInstanceCount_++;
            const uint32_t base = uint32_t(vertices.size());
            for (size_t v = 0; v < chunk.hullPositions.size(); ++v) {
                vertices.push_back(
                    {chunk.hullPositions[v], chunk.hullNormals[v], chunkId});
            }
            for (uint32_t index : chunk.hullIndices) {
                indices.push_back(base + index);
            }
        }
        draw.indexCount = UINT(indices.size()) - draw.indexOffset;
        if (draw.indexCount > 0) {
            chunkDraws_.push_back(draw);
        }
    }
    if (vertices.empty() || indices.empty()) {
        return true;
    }

    if (!rhiDevice_) {
        return false;
    }
    rhi::BufferDesc vertexDesc{};
    vertexDesc.size = vertices.size() * sizeof(ChunkVertex);
    vertexDesc.usage = rhi::BufferUsage::Vertex;
    vertexDesc.initialData = vertices.data();
    chunkVertexBuffer_ = rhiDevice_->CreateBuffer(vertexDesc);
    if (!chunkVertexBuffer_) {
        LogError("Voronoi chunk vertex buffer creation failed.");
        return false;
    }

    rhi::BufferDesc indexDesc{};
    indexDesc.size = indices.size() * sizeof(uint32_t);
    indexDesc.usage = rhi::BufferUsage::Index;
    indexDesc.initialData = indices.data();
    chunkIndexBuffer_ = rhiDevice_->CreateBuffer(indexDesc);
    if (!chunkIndexBuffer_) {
        LogError("Voronoi chunk index buffer creation failed.");
        return false;
    }

    chunkInstances_.assign(chunkInstanceCount_, Instance{});
    return EnsureChunkInstanceCapacity(std::max(64u, chunkInstanceCount_));
}

bool SceneRenderer::EnsureChunkInstanceCapacity(UINT capacity) {
    if (!rhiDevice_) {
        return false;
    }
    if (chunkInstanceBuffer_ && capacity <= chunkInstanceCapacity_) {
        return true;
    }
    capacity = std::max({capacity, 64u, chunkInstanceCapacity_ * 2u});

    rhi::BufferDesc desc{};
    desc.size = uint64_t(capacity) * sizeof(Instance);
    desc.usage = rhi::BufferUsage::ShaderRead | rhi::BufferUsage::Dynamic;
    desc.structureStride = sizeof(Instance);
    std::unique_ptr<rhi::Buffer> buffer = rhiDevice_->CreateBuffer(desc);
    if (!buffer) {
        return false;
    }
    chunkInstanceBuffer_ = std::move(buffer);
    chunkInstanceCapacity_ = capacity;
    return true;
}

void SceneRenderer::UpdateChunkInstances(ID3D11DeviceContext* context,
                                         const scene::Scene& scene) {
    (void)context;
    if (!chunkInstanceBuffer_ || chunkInstances_.empty() || chunkDraws_.empty()) {
        return;
    }
    const auto& bodies = scene.Physics().Bodies();
    UINT index = 0;
    for (const scene::SceneObject& object : scene.Objects()) {
        if (object.chunks.empty() || object.chunks.front().hullPositions.empty()) {
            continue;
        }
        for (const scene::SceneChunk& chunk : object.chunks) {
            if (chunk.hullPositions.empty() ||
                index >= UINT(chunkInstances_.size())) {
                continue;
            }
            Instance& instance = chunkInstances_[index++];
            XMMATRIX world = XMMatrixIdentity();
            if (chunk.bodyIndex >= 0 && chunk.bodyIndex < int(bodies.size())) {
                const scene::RigidBody& body = bodies[chunk.bodyIndex];
                // The hull is stored in building space and the collision box
                // has its own fitted orientation, so the visual motion is the
                // rotation relative to that fracture pose.
                const XMMATRIX initial = XMMatrixRotationQuaternion(
                    XMLoadFloat4(&chunk.orientation));
                const XMMATRIX current = XMMatrixRotationQuaternion(
                    XMLoadFloat4(&body.orientation));
                world = XMMatrixTranslation(-chunk.center.x, -chunk.center.y,
                                            -chunk.center.z) *
                        XMMatrixTranspose(initial) * current *
                        XMMatrixTranslation(body.position.x, body.position.y,
                                            body.position.z);
            }
            XMStoreFloat4x4(&instance.transform, world);
            instance.color = chunk.color;
            instance.params =
                XMFLOAT4(object.damageAmount * 0.7f, 0.0f, 0.0f, 3.0f);
            instance.state = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
            // The vertices are already in building space.
            instance.textureOrigin = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
            instance.textureScale = XMFLOAT4(1.0f, 1.0f, 1.0f,
                                             float(object.meshVariant & 3));
        }
    }
    if (index == 0) {
        return;
    }

    void* mapped = chunkInstanceBuffer_->Map();
    if (!mapped) {
        return;
    }
    std::memcpy(mapped, chunkInstances_.data(), size_t(index) * sizeof(Instance));
    chunkInstanceBuffer_->Unmap();
}

void SceneRenderer::DrawChunks(ID3D11DeviceContext* context,
                               const scene::Scene& scene, bool shadowPass) {
    if (!context || chunkDraws_.empty() || !chunkVertexBuffer_ ||
        !chunkIndexBuffer_ || !chunkInstanceBuffer_ ||
        !chunkInstanceBuffer_->NativeShaderResourceView()) {
        return;
    }
    const auto& objects = scene.Objects();
    const UINT stride = sizeof(ChunkVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertexBuffer =
        static_cast<ID3D11Buffer*>(chunkVertexBuffer_->NativeHandle());
    context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(static_cast<ID3D11Buffer*>(chunkIndexBuffer_->NativeHandle()),
                              DXGI_FORMAT_R32_UINT, 0);
    rhiDevice_->BindPipeline(shadowPass ? chunkShadowPipeline_.get()
                                        : chunkPipeline_.get());
    ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(
        chunkInstanceBuffer_->NativeShaderResourceView());
    context->VSSetShaderResources(1, 1, &srv);
    for (const ChunkDraw& draw : chunkDraws_) {
        if (draw.objectIndex < 0 || draw.objectIndex >= int(objects.size()) ||
            !objects[draw.objectIndex].collapsed) {
            continue;
        }
        context->DrawIndexed(draw.indexCount, draw.indexOffset, 0);
    }
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->VSSetShaderResources(1, 1, &nullSrv);
}

bool SceneRenderer::EnsureStaticInstanceCapacity(UINT capacity) {
    if (!rhiDevice_) {
        return false;
    }
    if (staticInstanceBuffer_ && capacity <= staticInstanceCapacity_) {
        return true;
    }
    capacity = std::max({capacity, 64u, staticInstanceCapacity_ * 2u});

    rhi::BufferDesc desc{};
    desc.size = uint64_t(capacity) * sizeof(Instance);
    desc.usage = rhi::BufferUsage::ShaderRead | rhi::BufferUsage::Dynamic;
    desc.structureStride = sizeof(Instance);
    std::unique_ptr<rhi::Buffer> buffer = rhiDevice_->CreateBuffer(desc);
    if (!buffer) {
        return false;
    }
    staticInstanceBuffer_ = std::move(buffer);
    staticInstanceCapacity_ = capacity;
    return true;
}

void SceneRenderer::RenderStaticShadow(ID3D11DeviceContext* context,
                                       const scene::Scene& scene,
                                       const XMFLOAT4X4& lightViewProj,
                                       uint64_t key) {
    if (!device_ || !context || !staticShadowDSV_) {
        return;
    }
    if (staticShadowValid_ && key == staticShadowKey_) {
        return;
    }
    staticShadowKey_ = key;
    staticShadowValid_ = true;

    // Always clear: a disabled or regenerated scene must not keep stale
    // building shadows.
    context->ClearDepthStencilView(staticShadowDSV_.Get(), D3D11_CLEAR_DEPTH,
                                   1.0f, 0);
    if (!scene.Settings().enabled || scene.Empty()) {
        return;
    }

    std::vector<Instance> instances;
    instances.reserve(scene.Objects().size());
    for (const scene::SceneObject& object : scene.Objects()) {
        if (object.type != scene::SceneObjectType::Building || object.collapsed) {
            continue;
        }
        Instance instance{};
        const XMMATRIX world =
            XMMatrixScaling(object.size.x, object.size.y, object.size.z) *
            XMMatrixRotationY(XMConvertToRadians(object.yaw)) *
            XMMatrixTranslation(object.position.x, 0.0f, object.position.z);
        XMStoreFloat4x4(&instance.transform, world);
        instances.push_back(instance);
    }
    if (instances.empty() ||
        !EnsureStaticInstanceCapacity(UINT(instances.size()))) {
        return;
    }

    void* mapped = staticInstanceBuffer_->Map();
    if (!mapped) {
        return;
    }
    std::memcpy(mapped, instances.data(), instances.size() * sizeof(Instance));
    staticInstanceBuffer_->Unmap();

    D3D11_VIEWPORT viewport{0.0f, 0.0f, float(kStaticShadowSize),
                            float(kStaticShadowSize), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(0, nullptr, staticShadowDSV_.Get());
    rhiDevice_->BindPipeline(sceneShadowPipeline_.get());

    void* cascadeMapped = shadowCascadeBuffer_->Map();
    if (cascadeMapped) {
        std::memcpy(cascadeMapped, &lightViewProj, sizeof(XMFLOAT4X4));
        shadowCascadeBuffer_->Unmap();
    }
    ID3D11Buffer* cascadeBuffer =
        static_cast<ID3D11Buffer*>(shadowCascadeBuffer_->NativeHandle());
    context->VSSetConstantBuffers(3, 1, &cascadeBuffer);

    const Mesh& mesh = meshes_[0];
    if (mesh.vertexBuffer && mesh.indexBuffer && mesh.indexCount > 0) {
        ID3D11Buffer* vertexBuffer = static_cast<ID3D11Buffer*>(mesh.vertexBuffer->NativeHandle());
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(static_cast<ID3D11Buffer*>(mesh.indexBuffer->NativeHandle()), DXGI_FORMAT_R32_UINT,
                                  0);
        ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(
            staticInstanceBuffer_->NativeShaderResourceView());
        context->VSSetShaderResources(0, 1, &srv);
        context->DrawIndexedInstanced(mesh.indexCount, UINT(instances.size()), 0,
                                      0, 0);
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->VSSetShaderResources(0, 1, &nullSrv);
    ID3D11Buffer* nullBuffer = nullptr;
    context->VSSetConstantBuffers(3, 1, &nullBuffer);
}

void SceneRenderer::RenderShadow(ID3D11DeviceContext* context,
                                 const scene::Scene& scene, const ViewInfo& view,
                                 const XMFLOAT4X4* lightViewProj,
                                 const XMFLOAT3& burstCenter) {
    if (!device_ || !context) {
        return;
    }
    // Always clear so the ground samples a fully lit map when the scene is off.
    for (int cascade = 0; cascade < kShadowCascades; ++cascade) {
        context->ClearDepthStencilView(shadowDSV_[cascade].Get(), D3D11_CLEAR_DEPTH,
                                       1.0f, 0);
    }
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
            meshIndex = kVehicleMeshFirst +
                        (object.meshVariant % scene::kVehicleModelCount);
            scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
            type = 1.0f;
            break;
        case scene::SceneObjectType::Tree: {
            const float dx = object.position.x - view.eye.x;
            const float dz = object.position.z - view.eye.z;
            const float distanceSquared = dx * dx + dz * dz;
            meshIndex = distanceSquared > 80.0f * 80.0f
                            ? kFarTreeMesh
                            : kTreeMeshFirst + (object.meshVariant % 3);
            scale = XMFLOAT3(object.size.x * 2.0f, object.size.y,
                             object.size.x * 2.0f);
            sway = 0.05f;
            type = 2.0f;
            break;
        }
        default:
            continue;
        }

        if (meshIndex < 0 || meshIndex >= int(meshes_.size())) {
            continue;
        }

        Instance instance{};
        XMMATRIX world;
        if (object.type == scene::SceneObjectType::Vehicle) {
            // Imported models keep their proportions: the object size is the
            // model size times a single uniform scale.
            const Mesh& mesh = meshes_[meshIndex];
            const float uniform = mesh.modelSize.z > 1e-4f
                                      ? object.size.z / mesh.modelSize.z
                                      : 1.0f;
            if (object.bodyIndex >= 0 && object.bodyIndex < int(bodies.size())) {
                const scene::RigidBody& body = bodies[object.bodyIndex];
                world = XMMatrixScaling(uniform, uniform, uniform) *
                        XMMatrixTranslation(0.0f, -mesh.modelSize.y * 0.5f, 0.0f) *
                        XMMatrixRotationQuaternion(XMLoadFloat4(&body.orientation)) *
                        XMMatrixTranslation(body.position.x, body.position.y,
                                            body.position.z);
            } else {
                world = XMMatrixScaling(uniform, uniform, uniform) *
                        XMMatrixRotationY(XMConvertToRadians(object.yaw)) *
                        XMMatrixTranslation(object.position.x, 0.0f,
                                            object.position.z);
            }
        } else if (object.bodyIndex >= 0 && object.bodyIndex < int(bodies.size())) {
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
        // The heat glow is directional: yzw is the unit vector from the object
        // centre to the burst, so the shader can light only the facing side.
        XMFLOAT3 blastDirection(0.0f, 0.0f, 1.0f);
        {
            const XMFLOAT3 toBlast(burstCenter.x - objectCenter.x,
                                   burstCenter.y - objectCenter.y,
                                   burstCenter.z - objectCenter.z);
            const float blastLength =
                std::sqrt(toBlast.x * toBlast.x + toBlast.y * toBlast.y +
                          toBlast.z * toBlast.z);
            if (blastLength > 1e-4f) {
                blastDirection = XMFLOAT3(toBlast.x / blastLength,
                                          toBlast.y / blastLength,
                                          toBlast.z / blastLength);
            }
        }
        instance.state = XMFLOAT4(object.burning ? 1.0f : 0.0f, blastDirection.x,
                                  blastDirection.y, blastDirection.z);
        // Building space for the facade (buildings are axis-aligned).
        instance.textureOrigin =
            XMFLOAT4(object.position.x, 0.0f, object.position.z, 0.0f);
        // Imported vehicles use their vertex colours; the 10+ tag tells the
        // shader to skip the procedural paint/glass/wheel pattern.
        const float styleTag =
            object.type == scene::SceneObjectType::Vehicle
                ? 10.0f + float(object.meshVariant % scene::kVehicleModelCount)
                : float(object.meshVariant & 3);
        instance.textureScale =
            XMFLOAT4(object.size.x, object.size.y, object.size.z, styleTag);
        meshes_[meshIndex].instances.push_back(instance);
    }

    // Building chunks: unit boxes driven by the rigid-body transforms, plus the
    // chunks that are still supported by the structural graph.
    // The static building map covers the whole town, so chunks are kept in the
    // dynamic shadow pass even when they are far outside the camera frustum.
    // At low sun angles their shadows can reach far into the visible area.
    const float shadowCasterMargin = std::max(200.0f, scene.Extent() * 2.0f);
    for (const scene::SceneObject& object : scene.Objects()) {
        if (!object.collapsed) {
            continue;
        }
        // Voronoi rubble is drawn through the dedicated chunk path.
        if (!object.chunks.empty() &&
            !object.chunks.front().hullPositions.empty()) {
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
            if (!visible(chunkCenter, chunkRadius + shadowCasterMargin)) {
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
            // The damage overlay is deliberately partial so the facade stays
            // readable on the rubble.
            instance.params =
                XMFLOAT4(object.damageAmount * 0.7f, 0.0f, 0.0f, 3.0f);
            instance.state = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
            // The chamfered box is a unit cube; map it back into building space
            // so every chunk keeps the facade of the part it broke off from.
            instance.textureOrigin =
                XMFLOAT4(chunk.center.x, chunk.center.y, chunk.center.z, 0.0f);
            instance.textureScale = XMFLOAT4(chunk.half.x * 2.0f, chunk.half.y * 2.0f,
                                             chunk.half.z * 2.0f,
                                             float(object.meshVariant & 3));
            meshes_[kChunkMesh].instances.push_back(instance);
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
        void* mapped = mesh.instanceBuffer->Map();
        if (!mapped) {
            continue;
        }
        std::memcpy(mapped, mesh.instances.data(),
                    mesh.instances.size() * sizeof(Instance));
        mesh.instanceBuffer->Unmap();
    }

    BuildChunkGeometry(scene);
    UpdateChunkInstances(context, scene);

    // --- Shadow pass: one draw per cascade -------------------------------
    rhiDevice_->BindPipeline(sceneShadowPipeline_.get());

    for (int cascade = 0; cascade < kShadowCascades; ++cascade) {
        context->OMSetRenderTargets(0, nullptr, shadowDSV_[cascade].Get());
        const float size = float(ShadowSize(cascade));
        D3D11_VIEWPORT shadowViewport{0.0f, 0.0f, size, size, 0.0f, 1.0f};
        context->RSSetViewports(1, &shadowViewport);

        void* mapped = shadowCascadeBuffer_->Map();
        if (mapped) {
            std::memcpy(mapped, &lightViewProj[cascade], sizeof(XMFLOAT4X4));
            shadowCascadeBuffer_->Unmap();
        }
        ID3D11Buffer* cascadeBuffer =
            static_cast<ID3D11Buffer*>(shadowCascadeBuffer_->NativeHandle());
        context->VSSetConstantBuffers(3, 1, &cascadeBuffer);

        // Intact buildings (mesh 0) are cached in the static shadow map; the
        // dynamic cascades contain debris, vehicles and trees in every cascade,
        // so rubble shadows match the building shadows at all distances.
        constexpr int kFirstDynamicMesh = 1;
        const int maxMesh = int(meshes_.size());
        for (int m = kFirstDynamicMesh; m < maxMesh; ++m) {
            Mesh& mesh = meshes_[m];
            if (mesh.instances.empty() || !mesh.instanceBuffer) {
                continue;
            }
            ID3D11Buffer* vertexBuffer = static_cast<ID3D11Buffer*>(mesh.vertexBuffer->NativeHandle());
            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
            context->IASetIndexBuffer(static_cast<ID3D11Buffer*>(mesh.indexBuffer->NativeHandle()), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(mesh.instanceBuffer->NativeShaderResourceView());
            context->VSSetShaderResources(0, 1, &srv);
            context->DrawIndexedInstanced(mesh.indexCount, UINT(mesh.instances.size()),
                                          0, 0, 0);
        }

        // Voronoi rubble (phase B chunks) with the chunk shadow shader.
        DrawChunks(context, scene, true);
        rhiDevice_->BindPipeline(sceneShadowPipeline_.get());
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->VSSetShaderResources(0, 1, &nullSrv);
    ID3D11Buffer* nullBuffer = nullptr;
    context->VSSetConstantBuffers(3, 1, &nullBuffer);
}

void SceneRenderer::RenderScene(ID3D11DeviceContext* context, const scene::Scene& scene,
                                ID3D11RenderTargetView* hdrRTV,
                                ID3D11DepthStencilView* depthDSV, int width,
                                int height, ID3D11SamplerState* surfaceSampler) {
    if (!device_ || !context || !hdrRTV || !depthDSV) {
        return;
    }
    if (!scene.Settings().enabled || scene.Empty()) {
        return;
    }

    context->OMSetRenderTargets(1, &hdrRTV, depthDSV);
    rhiDevice_->BindPipeline(scenePipeline_.get());

    D3D11_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);

    ID3D11ShaderResourceView* shadowViews[4] = {
        shadowSRV_[0].Get(), shadowSRV_[1].Get(), shadowSRV_[2].Get(),
        staticShadowSRV_.Get()};
    context->PSSetShaderResources(2, 4, shadowViews);
    ID3D11SamplerState* shadowSampler =
        static_cast<ID3D11SamplerState*>(shadowSampler_->NativeHandle());
    context->PSSetSamplers(2, 1, &shadowSampler);

    // Tiling grain for the procedural materials. The sampler is chosen by the
    // anisotropic-filtering setting, so objects benefit from it as well.
    ID3D11ShaderResourceView* surfaceDetail = surfaceDetailSRV_.Get();
    context->PSSetShaderResources(6, 1, &surfaceDetail);
    ID3D11SamplerState* detailSampler = surfaceSampler;
    context->PSSetSamplers(3, 1, &detailSampler);

    for (Mesh& mesh : meshes_) {
        if (mesh.instances.empty() || !mesh.instanceBuffer) {
            continue;
        }
        ID3D11Buffer* vertexBuffer = static_cast<ID3D11Buffer*>(mesh.vertexBuffer->NativeHandle());
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetIndexBuffer(static_cast<ID3D11Buffer*>(mesh.indexBuffer->NativeHandle()), DXGI_FORMAT_R32_UINT, 0);
        ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(mesh.instanceBuffer->NativeShaderResourceView());
        context->VSSetShaderResources(0, 1, &srv);
        context->DrawIndexedInstanced(mesh.indexCount, UINT(mesh.instances.size()), 0, 0,
                                      0);
    }

    // Voronoi rubble with the chunk vertex shader and the shared pixel shader.
    DrawChunks(context, scene, false);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11ShaderResourceView* nullShadow[4] = {nullptr, nullptr, nullptr,
                                               nullptr};
    context->PSSetShaderResources(2, 4, nullShadow);
    ID3D11ShaderResourceView* nullDetail = nullptr;
    context->PSSetShaderResources(6, 1, &nullDetail);
    ID3D11SamplerState* nullSampler = nullptr;
    context->PSSetSamplers(2, 1, &nullSampler);
    context->PSSetSamplers(3, 1, &nullSampler);
    context->VSSetShaderResources(0, 1, &nullSrv);
}

} // namespace ex::gfx
