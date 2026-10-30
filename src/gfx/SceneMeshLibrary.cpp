#include "gfx/SceneMeshLibrary.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace ex::gfx {

namespace {

using Vertex = SceneMeshVertex;

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

XMFLOAT3 Normalize3(const XMFLOAT3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-6f) {
        return XMFLOAT3(0.0f, 1.0f, 0.0f);
    }
    return XMFLOAT3(v.x / length, v.y / length, v.z / length);
}

void AddQuadOriented(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                     const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                     const XMFLOAT3& d, const XMFLOAT3& normal,
                     const XMFLOAT3& color) {
    auto emit = [&](const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2) {
        const XMFLOAT3 e1(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
        const XMFLOAT3 e2(p2.x - p0.x, p2.y - p0.y, p2.z - p0.z);
        const XMFLOAT3 geometric(e1.y * e2.z - e1.z * e2.y,
                                 e1.z * e2.x - e1.x * e2.z,
                                 e1.x * e2.y - e1.y * e2.x);
        if (geometric.x * normal.x + geometric.y * normal.y +
                geometric.z * normal.z <
            0.0f) {
            AddTriangle(vertices, indices, p0, p2, p1, normal, color);
        } else {
            AddTriangle(vertices, indices, p0, p1, p2, normal, color);
        }
    };
    emit(a, b, c);
    emit(a, c, d);
}

void AddTriangleOriented(std::vector<Vertex>& vertices,
                         std::vector<uint32_t>& indices, const XMFLOAT3& a,
                         const XMFLOAT3& b, const XMFLOAT3& c,
                         const XMFLOAT3& normal, const XMFLOAT3& color) {
    const XMFLOAT3 e1(b.x - a.x, b.y - a.y, b.z - a.z);
    const XMFLOAT3 e2(c.x - a.x, c.y - a.y, c.z - a.z);
    const XMFLOAT3 geometric(e1.y * e2.z - e1.z * e2.y,
                             e1.z * e2.x - e1.x * e2.z,
                             e1.x * e2.y - e1.y * e2.x);
    if (geometric.x * normal.x + geometric.y * normal.y + geometric.z * normal.z <
        0.0f) {
        AddTriangle(vertices, indices, a, c, b, normal, color);
    } else {
        AddTriangle(vertices, indices, a, b, c, normal, color);
    }
}

} // namespace

void MeshAddBox(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
            const XMFLOAT3& center, const XMFLOAT3& half,
            const XMFLOAT3& color) {
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
        {corner(-1, -1, 1), corner(-1, -1, -1), corner(-1, 1, -1),
         corner(-1, 1, 1), {-1, 0, 0}},
        {corner(-1, 1, -1), corner(1, 1, -1), corner(1, 1, 1), corner(-1, 1, 1),
         {0, 1, 0}},
        {corner(-1, -1, 1), corner(1, -1, 1), corner(1, -1, -1),
         corner(-1, -1, -1), {0, -1, 0}},
        {corner(-1, -1, 1), corner(-1, 1, 1), corner(1, 1, 1), corner(1, -1, 1),
         {0, 0, 1}},
        {corner(1, -1, -1), corner(1, 1, -1), corner(-1, 1, -1),
         corner(-1, -1, -1), {0, 0, -1}},
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

void MeshAddBeveledBox(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                   const XMFLOAT3& half, float bevel, const XMFLOAT3& color) {
    const float b =
        std::min({bevel, half.x * 0.35f, half.y * 0.35f, half.z * 0.35f});
    const float hx = half.x;
    const float hy = half.y;
    const float hz = half.z;

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

    for (int s0 = -1; s0 <= 1; s0 += 2) {
        for (int s1 = -1; s1 <= 1; s1 += 2) {
            {
                const XMFLOAT3 a = vertex(0, float(s0), float(s1), -1.0f);
                const XMFLOAT3 b2 = vertex(0, float(s0), float(s1), 1.0f);
                const XMFLOAT3 c = vertex(1, float(s0), float(s1), 1.0f);
                const XMFLOAT3 d = vertex(1, float(s0), float(s1), -1.0f);
                AddQuadOriented(vertices, indices, a, b2, c, d,
                                Normalize3(XMFLOAT3(float(s0), float(s1), 0.0f)),
                                color);
            }
            {
                const XMFLOAT3 a = vertex(0, float(s0), -1.0f, float(s1));
                const XMFLOAT3 b2 = vertex(0, float(s0), 1.0f, float(s1));
                const XMFLOAT3 c = vertex(2, float(s0), 1.0f, float(s1));
                const XMFLOAT3 d = vertex(2, float(s0), -1.0f, float(s1));
                AddQuadOriented(vertices, indices, a, b2, c, d,
                                Normalize3(XMFLOAT3(float(s0), 0.0f, float(s1))),
                                color);
            }
            {
                const XMFLOAT3 a = vertex(1, -1.0f, float(s0), float(s1));
                const XMFLOAT3 b2 = vertex(1, 1.0f, float(s0), float(s1));
                const XMFLOAT3 c = vertex(2, 1.0f, float(s0), float(s1));
                const XMFLOAT3 d = vertex(2, -1.0f, float(s0), float(s1));
                AddQuadOriented(vertices, indices, a, b2, c, d,
                                Normalize3(XMFLOAT3(0.0f, float(s0), float(s1))),
                                color);
            }
        }
    }

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

void MeshAddCone(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
             const XMFLOAT3& baseCenter, float radius, float height,
             const XMFLOAT3& color, int segments) {
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
        AddTriangle(vertices, indices, baseCenter, b1, b0, XMFLOAT3(0, -1, 0),
                    color);
    }
}

SceneMeshData MakeBuildingMesh() {
    SceneMeshData mesh;
    MeshAddBox(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.5f, 0.0f),
           XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(1.0f, 1.0f, 1.0f));
    return mesh;
}

SceneMeshData MakeVehiclePlaceholderMesh() {
    SceneMeshData mesh;
    MeshAddBox(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.65f, 0.0f),
           XMFLOAT3(0.75f, 0.65f, 1.40f), XMFLOAT3(1.0f, 1.0f, 1.0f));
    return mesh;
}

SceneMeshData MakeTreeMesh(int variant) {
    SceneMeshData mesh;
    const XMFLOAT3 brown(0.22f, 0.14f, 0.08f);
    if (variant == 0) {
        MeshAddBox(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.25f, 0.0f),
               XMFLOAT3(0.04f, 0.25f, 0.04f), brown);
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.32f, 0.0f), 0.50f,
                0.48f, XMFLOAT3(0.12f, 0.30f, 0.10f));
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.52f, 0.0f), 0.38f,
                0.36f, XMFLOAT3(0.16f, 0.36f, 0.12f));
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.70f, 0.0f), 0.25f,
                0.30f, XMFLOAT3(0.20f, 0.40f, 0.14f));
    } else if (variant == 1) {
        MeshAddBox(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.30f, 0.0f),
               XMFLOAT3(0.035f, 0.30f, 0.035f), brown);
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.35f, 0.0f), 0.40f,
                0.40f, XMFLOAT3(0.10f, 0.28f, 0.09f));
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.55f, 0.0f), 0.30f,
                0.35f, XMFLOAT3(0.15f, 0.34f, 0.11f));
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.72f, 0.0f), 0.20f,
                0.28f, XMFLOAT3(0.19f, 0.38f, 0.13f));
    } else {
        MeshAddBox(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.20f, 0.0f),
               XMFLOAT3(0.05f, 0.20f, 0.05f), brown);
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.25f, 0.0f), 0.50f,
                0.45f, XMFLOAT3(0.14f, 0.32f, 0.11f));
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.45f, 0.0f), 0.42f,
                0.35f, XMFLOAT3(0.18f, 0.38f, 0.13f));
        MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.62f, 0.0f), 0.30f,
                0.30f, XMFLOAT3(0.22f, 0.42f, 0.15f));
    }
    return mesh;
}

SceneMeshData MakeFarTreeMesh() {
    SceneMeshData mesh;
    MeshAddCone(mesh.vertices, mesh.indices, XMFLOAT3(0.0f, 0.25f, 0.0f), 0.48f,
            0.75f, XMFLOAT3(0.14f, 0.33f, 0.11f));
    return mesh;
}

SceneMeshData MakeChunkMesh() {
    SceneMeshData mesh;
    MeshAddBeveledBox(mesh.vertices, mesh.indices, XMFLOAT3(0.5f, 0.5f, 0.5f), 0.10f,
                  XMFLOAT3(1.0f, 1.0f, 1.0f));
    return mesh;
}

} // namespace ex::gfx
