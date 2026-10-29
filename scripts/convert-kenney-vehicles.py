"""Convert Kenney Car Kit OBJ models into the compact ESV mesh format.

Source: https://kenney.nl/assets/car-kit (CC0 1.0)
The OBJ files reference a shared 512x512 colour atlas. The runtime renderer
uses per-vertex colours instead of textures, so every face is flat-shaded and
its colour is sampled from the atlas at the face UV centroid.

Usage:
    python scripts/convert-kenney-vehicles.py <obj_dir> <out_dir>

<obj_dir> must contain the Kenney OBJ files and Textures/colormap.png.
"""

import os
import struct
import sys

from PIL import Image

MODELS = [
    "sedan",
    "sedan-sports",
    "hatchback-sports",
    "suv",
    "suv-luxury",
    "van",
    "taxi",
    "police",
    "truck",
    "truck-flat",
]


def srgb_to_linear(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def parse_obj(path):
    positions = []
    uvs = []
    faces = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                positions.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("vt "):
                p = line.split()
                uvs.append((float(p[1]), float(p[2])))
            elif line.startswith("f "):
                verts = []
                for token in line.split()[1:]:
                    parts = token.split("/")
                    vi = int(parts[0]) - 1
                    ti = int(parts[1]) - 1 if len(parts) > 1 and parts[1] else -1
                    verts.append((vi, ti))
                if len(verts) >= 3:
                    faces.append(verts)
    return positions, uvs, faces


def sample_atlas(atlas, u, v):
    w, h = atlas.size
    # OBJ UV origin is bottom-left; the PNG is top-left.
    x = min(max(int(round(u * (w - 1))), 0), w - 1)
    y = min(max(int(round((1.0 - v) * (h - 1))), 0), h - 1)
    r, g, b = atlas.getpixel((x, y))[:3]
    return (srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b))


def convert(name, src_dir, out_dir, atlas):
    positions, uvs, faces = parse_obj(os.path.join(src_dir, name + ".obj"))
    if not positions or not faces:
        raise RuntimeError(f"{name}: no geometry")

    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    min_z, max_z = min(zs), max(zs)
    cx = 0.5 * (min_x + max_x)
    cz = 0.5 * (min_z + max_z)
    width = max_x - min_x
    height = max_y - min_y
    length = max_z - min_z

    out_vertices = []
    out_indices = []

    def add_vertex(pos, normal, color):
        out_vertices.append((pos[0], pos[1], pos[2],
                             normal[0], normal[1], normal[2],
                             color[0], color[1], color[2]))
        return len(out_vertices) - 1

    for face in faces:
        for i in range(1, len(face) - 1):
            tri = [face[0], face[i], face[i + 1]]
            p0 = positions[tri[0][0]]
            p1 = positions[tri[1][0]]
            p2 = positions[tri[2][0]]
            # Face normal from the triangle.
            ux, uy, uz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
            vx, vy, vz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            ln = (nx * nx + ny * ny + nz * nz) ** 0.5
            if ln < 1e-8:
                nx, ny, nz, ln = 0.0, 1.0, 0.0, 1.0
            normal = (nx / ln, ny / ln, nz / ln)

            # Sample the atlas at the UV centroid of the triangle.
            u_sum = 0.0
            v_sum = 0.0
            uv_count = 0
            for vi, ti in tri:
                if 0 <= ti < len(uvs):
                    u_sum += uvs[ti][0]
                    v_sum += uvs[ti][1]
                    uv_count += 1
            if uv_count:
                color = sample_atlas(atlas, u_sum / uv_count, v_sum / uv_count)
            else:
                color = (0.5, 0.5, 0.5)

            idx = []
            for vi, _ in tri:
                p = positions[vi]
                idx.append(add_vertex((p[0] - cx, p[1] - min_y, p[2] - cz),
                                      normal, color))
            out_indices.extend(idx)

    header = struct.pack("<4sIIfff", b"ESV1", len(out_vertices),
                         len(out_indices), width, height, length)
    with open(os.path.join(out_dir, name + ".esv"), "wb") as f:
        f.write(header)
        for v in out_vertices:
            f.write(struct.pack("<9f", *v))
        f.write(struct.pack("<%dI" % len(out_indices), *out_indices))

    print(f"{name:18s} {len(out_vertices):6d} verts  {len(out_indices):6d} idx  "
          f"{width:.2f} x {height:.2f} x {length:.2f} m")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    atlas = Image.open(os.path.join(src_dir, "Textures", "colormap.png")).convert("RGB")
    for name in MODELS:
        convert(name, src_dir, out_dir, atlas)
    return 0


if __name__ == "__main__":
    sys.exit(main())
