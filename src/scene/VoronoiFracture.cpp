#include "scene/VoronoiFracture.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace ex::scene {
namespace {

struct Polyhedron {
    std::vector<XMFLOAT3> vertices;
    std::vector<std::vector<int>> faces;
    std::vector<int> faceSeed; // seed index that produced the face, -1 for the box
};

struct Rng {
    uint64_t state;

    uint64_t Next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Float01() { return float(Next() >> 40) * (1.0f / 16777216.0f); }
    float Range(float a, float b) { return a + (b - a) * Float01(); }
};

XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}
XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}
XMFLOAT3 Scale(const XMFLOAT3& a, float s) {
    return XMFLOAT3(a.x * s, a.y * s, a.z * s);
}
float Dot(const XMFLOAT3& a, const XMFLOAT3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x);
}
float LengthSq(const XMFLOAT3& a) { return Dot(a, a); }
XMFLOAT3 Normalize(const XMFLOAT3& a) {
    const float length = std::sqrt(LengthSq(a));
    if (length < 1e-12f) {
        return XMFLOAT3(0.0f, 1.0f, 0.0f);
    }
    return Scale(a, 1.0f / length);
}
XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
    return Add(a, Scale(Sub(b, a), t));
}

int AddVertex(Polyhedron& poly, const XMFLOAT3& point) {
    for (int i = 0; i < int(poly.vertices.size()); ++i) {
        if (LengthSq(Sub(poly.vertices[i], point)) < 1e-10f) {
            return i;
        }
    }
    poly.vertices.push_back(point);
    return int(poly.vertices.size()) - 1;
}

// Clips the convex polyhedron to the half-space `dot(n, x) <= d`. The cap face
// is attributed to `seed`, which yields the face adjacency of the cells.
void ClipByPlane(Polyhedron& poly, const XMFLOAT3& normal, float d, int seed) {
    constexpr float kEpsilon = 1e-5f;

    std::vector<float> distance(poly.vertices.size());
    bool anyInside = false;
    bool anyOutside = false;
    for (size_t i = 0; i < poly.vertices.size(); ++i) {
        distance[i] = Dot(normal, poly.vertices[i]) - d;
        if (distance[i] <= kEpsilon) {
            anyInside = true;
        } else {
            anyOutside = true;
        }
    }
    if (!anyInside || !anyOutside) {
        return;
    }

    Polyhedron out;
    std::vector<int> cap;
    for (size_t f = 0; f < poly.faces.size(); ++f) {
        const std::vector<int>& face = poly.faces[f];
        if (face.size() < 3) {
            continue;
        }
        std::vector<int> clipped;
        const int count = int(face.size());
        for (int i = 0; i < count; ++i) {
            const int ia = face[i];
            const int ib = face[(i + 1) % count];
            const bool insideA = distance[ia] <= kEpsilon;
            const bool insideB = distance[ib] <= kEpsilon;
            if (insideA) {
                clipped.push_back(AddVertex(out, poly.vertices[ia]));
            }
            if (insideA != insideB) {
                const float t = distance[ia] / (distance[ia] - distance[ib]);
                const int index = AddVertex(out, Lerp(poly.vertices[ia],
                                                      poly.vertices[ib], t));
                clipped.push_back(index);
                cap.push_back(index);
            }
        }
        // Remove repeated indices from the clipped polygon (can happen when a
        // vertex lies exactly on the plane).
        std::vector<int> clean;
        for (int index : clipped) {
            if (clean.empty() || clean.back() != index) {
                clean.push_back(index);
            }
        }
        while (clean.size() > 1 && clean.front() == clean.back()) {
            clean.pop_back();
        }
        if (clean.size() >= 3) {
            out.faces.push_back(std::move(clean));
            out.faceSeed.push_back(poly.faceSeed[f]);
        }
    }

    std::vector<int> uniqueCap;
    for (int index : cap) {
        if (std::find(uniqueCap.begin(), uniqueCap.end(), index) ==
            uniqueCap.end()) {
            uniqueCap.push_back(index);
        }
    }
    if (uniqueCap.size() >= 3) {
        XMFLOAT3 centroid(0.0f, 0.0f, 0.0f);
        for (int index : uniqueCap) {
            centroid = Add(centroid, out.vertices[index]);
        }
        centroid = Scale(centroid, 1.0f / float(uniqueCap.size()));

        // Build a basis with u x v = normal so that increasing angles are CCW
        // when the face is viewed from outside.
        const XMFLOAT3 reference =
            std::abs(normal.x) < 0.9f ? XMFLOAT3(1.0f, 0.0f, 0.0f)
                                      : XMFLOAT3(0.0f, 1.0f, 0.0f);
        const XMFLOAT3 u = Normalize(Sub(reference, Scale(normal,
                                                          Dot(reference, normal))));
        const XMFLOAT3 v = Cross(normal, u);
        std::sort(uniqueCap.begin(), uniqueCap.end(), [&](int a, int b) {
            const XMFLOAT3 pa = Sub(out.vertices[a], centroid);
            const XMFLOAT3 pb = Sub(out.vertices[b], centroid);
            return std::atan2(Dot(pa, v), Dot(pa, u)) <
                   std::atan2(Dot(pb, v), Dot(pb, u));
        });
        out.faces.push_back(std::move(uniqueCap));
        out.faceSeed.push_back(seed);
    }

    poly = std::move(out);
}

XMFLOAT3 FaceNormal(const Polyhedron& poly, const std::vector<int>& face) {
    XMFLOAT3 normal(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < face.size(); ++i) {
        const XMFLOAT3& current = poly.vertices[face[i]];
        const XMFLOAT3& next = poly.vertices[face[(i + 1) % face.size()]];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }
    return Normalize(normal);
}

float FaceArea(const Polyhedron& poly, const std::vector<int>& face) {
    float area = 0.0f;
    for (size_t i = 1; i + 1 < face.size(); ++i) {
        const XMFLOAT3& a = poly.vertices[face[0]];
        const XMFLOAT3& b = poly.vertices[face[i]];
        const XMFLOAT3& c = poly.vertices[face[i + 1]];
        area += 0.5f * std::sqrt(LengthSq(Cross(Sub(b, a), Sub(c, a))));
    }
    return area;
}

// Jacobi eigenvalue iteration for a symmetric 3x3 matrix. Eigenvectors are
// returned as the columns of `v`.
void JacobiEigen(double a[3][3], double v[3][3]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            v[i][j] = i == j ? 1.0 : 0.0;
        }
    }
    for (int iteration = 0; iteration < 32; ++iteration) {
        int p = 0;
        int q = 1;
        double maximum = std::abs(a[0][1]);
        if (std::abs(a[0][2]) > maximum) {
            p = 0;
            q = 2;
            maximum = std::abs(a[0][2]);
        }
        if (std::abs(a[1][2]) > maximum) {
            p = 1;
            q = 2;
            maximum = std::abs(a[1][2]);
        }
        if (maximum < 1e-12) {
            break;
        }
        const double theta =
            0.5 * std::atan2(2.0 * a[p][q], a[q][q] - a[p][p]);
        const double c = std::cos(theta);
        const double s = std::sin(theta);
        const double app = a[p][p];
        const double aqq = a[q][q];
        const double apq = a[p][q];
        a[p][p] = c * c * app - 2.0 * s * c * apq + s * s * aqq;
        a[q][q] = s * s * app + 2.0 * s * c * apq + c * c * aqq;
        a[p][q] = a[q][p] = 0.0;
        for (int k = 0; k < 3; ++k) {
            if (k == p || k == q) {
                continue;
            }
            const double akp = a[k][p];
            const double akq = a[k][q];
            a[k][p] = a[p][k] = c * akp - s * akq;
            a[k][q] = a[q][k] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
            const double vkp = v[k][p];
            const double vkq = v[k][q];
            v[k][p] = c * vkp - s * vkq;
            v[k][q] = s * vkp + c * vkq;
        }
    }
}

// Fits an oriented box to the cell vertices with a principal-component frame.
void FitOrientedBox(const std::vector<XMFLOAT3>& points, XMFLOAT3& center,
                    XMFLOAT3& half, XMFLOAT4& orientation) {
    // The hull is flat-shaded, so vertices are duplicated per face. Deduplicate
    // them first so the covariance is not biased towards large faces.
    std::vector<XMFLOAT3> unique;
    unique.reserve(points.size());
    for (const XMFLOAT3& point : points) {
        bool found = false;
        for (const XMFLOAT3& existing : unique) {
            if (LengthSq(Sub(existing, point)) < 1e-10f) {
                found = true;
                break;
            }
        }
        if (!found) {
            unique.push_back(point);
        }
    }
    if (unique.empty()) {
        unique = points;
    }

    XMFLOAT3 mean(0.0f, 0.0f, 0.0f);
    for (const XMFLOAT3& point : unique) {
        mean = Add(mean, point);
    }
    mean = Scale(mean, 1.0f / float(unique.size()));

    double covariance[3][3] = {};
    for (const XMFLOAT3& point : unique) {
        const double x = point.x - mean.x;
        const double y = point.y - mean.y;
        const double z = point.z - mean.z;
        covariance[0][0] += x * x;
        covariance[0][1] += x * y;
        covariance[0][2] += x * z;
        covariance[1][1] += y * y;
        covariance[1][2] += y * z;
        covariance[2][2] += z * z;
    }
    covariance[1][0] = covariance[0][1];
    covariance[2][0] = covariance[0][2];
    covariance[2][1] = covariance[1][2];

    double vectors[3][3];
    JacobiEigen(covariance, vectors);

    XMFLOAT3 axis0 = Normalize(
        XMFLOAT3(float(vectors[0][0]), float(vectors[1][0]), float(vectors[2][0])));
    XMFLOAT3 axis1 =
        XMFLOAT3(float(vectors[0][1]), float(vectors[1][1]), float(vectors[2][1]));
    axis1 = Normalize(Sub(axis1, Scale(axis0, Dot(axis1, axis0))));
    XMFLOAT3 axis2 = Normalize(Cross(axis0, axis1));
    axis1 = Normalize(Cross(axis2, axis0));

    float minimum[3] = {1e30f, 1e30f, 1e30f};
    float maximum[3] = {-1e30f, -1e30f, -1e30f};
    const XMFLOAT3 axes[3] = {axis0, axis1, axis2};
    for (const XMFLOAT3& point : unique) {
        for (int axis = 0; axis < 3; ++axis) {
            const float projection = Dot(point, axes[axis]);
            minimum[axis] = std::min(minimum[axis], projection);
            maximum[axis] = std::max(maximum[axis], projection);
        }
    }

    center = XMFLOAT3(0.0f, 0.0f, 0.0f);
    float extents[3] = {0.0f, 0.0f, 0.0f};
    for (int axis = 0; axis < 3; ++axis) {
        const float middle = 0.5f * (minimum[axis] + maximum[axis]);
        center = Add(center, Scale(axes[axis], middle));
        extents[axis] = std::max(0.02f, 0.5f * (maximum[axis] - minimum[axis]));
    }
    half = XMFLOAT3(extents[0], extents[1], extents[2]);

    const XMMATRIX matrix =
        XMMatrixSet(axis0.x, axis0.y, axis0.z, 0.0f, axis1.x, axis1.y, axis1.z,
                    0.0f, axis2.x, axis2.y, axis2.z, 0.0f, 0.0f, 0.0f, 0.0f,
                    1.0f);
    XMStoreFloat4(&orientation, XMQuaternionRotationMatrix(matrix));
}

Polyhedron MakeBox(const XMFLOAT3& center, const XMFLOAT3& half) {
    Polyhedron poly;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                poly.vertices.push_back(
                    XMFLOAT3(center.x + float(sx) * half.x,
                             center.y + float(sy) * half.y,
                             center.z + float(sz) * half.z));
            }
        }
    }
    auto index = [](int sx, int sy, int sz) {
        return ((sx + 1) / 2) * 4 + ((sy + 1) / 2) * 2 + (sz + 1) / 2;
    };
    // Counter-clockwise when seen from outside.
    const int faces[6][4] = {
        {index(1, -1, -1), index(1, 1, -1), index(1, 1, 1), index(1, -1, 1)},
        {index(-1, -1, 1), index(-1, 1, 1), index(-1, 1, -1), index(-1, -1, -1)},
        {index(-1, 1, -1), index(-1, 1, 1), index(1, 1, 1), index(1, 1, -1)},
        {index(-1, -1, 1), index(-1, -1, -1), index(1, -1, -1), index(1, -1, 1)},
        {index(-1, -1, 1), index(1, -1, 1), index(1, 1, 1), index(-1, 1, 1)},
        {index(1, -1, -1), index(-1, -1, -1), index(-1, 1, -1), index(1, 1, -1)},
    };
    for (const auto& face : faces) {
        poly.faces.push_back({face[0], face[1], face[2], face[3]});
        poly.faceSeed.push_back(-1);
    }
    return poly;
}

} // namespace

// Clips the box into one Voronoi cell per seed and fills the hull, the fitted
// collision proxy and the face-seed lists.
void BuildCells(const std::vector<XMFLOAT3>& seeds, const Polyhedron& box,
                std::vector<FractureCell>& cells,
                std::vector<std::vector<int>>& faceSeeds) {
    cells.assign(seeds.size(), FractureCell{});
    faceSeeds.assign(seeds.size(), {});

    for (int i = 0; i < int(seeds.size()); ++i) {
        Polyhedron poly = box;
        for (int j = 0; j < int(seeds.size()); ++j) {
            if (j == i) {
                continue;
            }
            const XMFLOAT3 normal = Normalize(Sub(seeds[j], seeds[i]));
            const float offset =
                Dot(normal, Scale(Add(seeds[i], seeds[j]), 0.5f));
            ClipByPlane(poly, normal, offset, j);
        }

        FractureCell& cell = cells[i];
        float minimumY = 1e30f;
        for (size_t f = 0; f < poly.faces.size(); ++f) {
            const std::vector<int>& face = poly.faces[f];
            if (face.size() < 3) {
                continue;
            }
            const float area = FaceArea(poly, face);
            if (area < 1e-4f) {
                continue;
            }
            const XMFLOAT3 normal = FaceNormal(poly, face);
            const uint32_t base = uint32_t(cell.positions.size());
            for (int index : face) {
                cell.positions.push_back(poly.vertices[index]);
                cell.normals.push_back(normal);
                minimumY = std::min(minimumY, poly.vertices[index].y);
            }
            for (size_t k = 1; k + 1 < face.size(); ++k) {
                cell.indices.push_back(base);
                cell.indices.push_back(base + uint32_t(k));
                cell.indices.push_back(base + uint32_t(k + 1));
            }
            if (poly.faceSeed[f] >= 0) {
                faceSeeds[i].push_back(poly.faceSeed[f]);
            }
            cell.surfaceArea += area;
        }

        // Volume of the convex hull (signed tetrahedra from the origin).
        float signedVolume = 0.0f;
        for (size_t t = 0; t + 2 < cell.indices.size(); t += 3) {
            const XMFLOAT3& a = cell.positions[cell.indices[t]];
            const XMFLOAT3& b = cell.positions[cell.indices[t + 1]];
            const XMFLOAT3& c = cell.positions[cell.indices[t + 2]];
            signedVolume += Dot(a, Cross(b, c));
        }
        cell.volume = std::abs(signedVolume) / 6.0f;
        cell.grounded = minimumY <= 0.03f;

        FitOrientedBox(cell.positions, cell.center, cell.half, cell.orientation);
    }
}

void FractureBoxVoronoi(const XMFLOAT3& center, const XMFLOAT3& half,
                        int cellCount, uint32_t seed,
                        std::vector<FractureCell>& cells) {
    cells.clear();
    if (cellCount < 2 || half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) {
        return;
    }

    Rng rng{uint64_t(seed) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull};
    const float volume = 8.0f * half.x * half.y * half.z;
    const float targetCellVolume = volume / float(cellCount);
    const float minimumDistance =
        0.55f * std::cbrt(std::max(targetCellVolume, 1e-3f));

    // Best-candidate sampling: each seed takes the candidate farthest from the
    // existing seeds, which avoids slivers without an expensive relaxation.
    std::vector<XMFLOAT3> seeds;
    seeds.reserve(cellCount);
    for (int i = 0; i < cellCount; ++i) {
        XMFLOAT3 best{};
        float bestDistance = -1.0f;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const XMFLOAT3 candidate(
                center.x + rng.Range(-half.x, half.x),
                center.y + rng.Range(-half.y, half.y),
                center.z + rng.Range(-half.z, half.z));
            float nearest = 1e30f;
            for (const XMFLOAT3& existing : seeds) {
                nearest = std::min(nearest, LengthSq(Sub(candidate, existing)));
            }
            if (nearest > bestDistance) {
                bestDistance = nearest;
                best = candidate;
            }
            if (bestDistance >= minimumDistance * minimumDistance) {
                break;
            }
        }
        seeds.push_back(best);
    }

    const Polyhedron box = MakeBox(center, half);
    std::vector<std::vector<int>> faceSeeds;

    // One Lloyd-style relaxation pass removes the worst slivers while keeping
    // the irregular cell shapes.
    BuildCells(seeds, box, cells, faceSeeds);
    for (int i = 0; i < int(seeds.size()); ++i) {
        seeds[i] = Lerp(seeds[i], cells[i].center, 0.8f);
    }
    BuildCells(seeds, box, cells, faceSeeds);

    // Face adjacency is symmetric: both cells see the shared bisector face.
    for (int i = 0; i < int(faceSeeds.size()); ++i) {
        for (int j : faceSeeds[i]) {
            if (j < 0 || j >= int(cells.size()) || j == i) {
                continue;
            }
            auto add = [](std::vector<int>& list, int value) {
                if (std::find(list.begin(), list.end(), value) == list.end()) {
                    list.push_back(value);
                }
            };
            add(cells[i].neighbors, j);
            add(cells[j].neighbors, i);
        }
    }

    // Guarantee at least one grounded cell so the support flood fill has a seed.
    bool anyGrounded = false;
    for (const FractureCell& cell : cells) {
        anyGrounded = anyGrounded || cell.grounded;
    }
    if (!anyGrounded && !cells.empty()) {
        int lowest = 0;
        for (int i = 1; i < int(cells.size()); ++i) {
            if (cells[i].center.y < cells[lowest].center.y) {
                lowest = i;
            }
        }
        cells[lowest].grounded = true;
    }
}

} // namespace ex::scene
