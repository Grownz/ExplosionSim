# Linux build

The portable simulation core and the Vulkan bring-up target already build and
run natively on Linux. The graphical application is still Windows-only until the
SDL3 platform layer and the Vulkan renderer land (milestones M2/M3 in
[`linux-port.md`](linux-port.md)).

## Current Linux targets

- `explosion_core`: blast model, particles, simulation, CPU volume bake, scene
  generation, Voronoi fracture, Jolt physics, thread pool.
- `core_tests`: portable smoke tests for the blast model, deterministic scene
  generation and the CPU volume bake.
- `vulkan_smoke`: Vulkan 1.2 device, offscreen target, clear, readback and
  (with a desktop) an SDL3 window with a swapchain. `--triangle` exercises
  vertex buffers, SPIR-V shader modules and a graphics pipeline; `--quad` adds
  a texture, sampler, uniform buffer and descriptor set; `--sky` runs the real
  `assets/shaders/sky.hlsl` with the shared frame constants; `--composite` runs
  the real tonemap/bloom composite with two sampled textures; `--ground` runs
  the ground plane with cascaded/static shadow maps and a 3D volume texture;
  `--target` renders a pass into an offscreen texture and samples it afterwards;
  `--bloom` runs the real downsample/upsample bloom chain into render targets;
  `--scene` draws an instanced cube through the real scene shader with
  structured instance buffers, shadow maps and surface detail; `--shock` runs
  the shock-front refraction pass with scene color and depth; `--volume` runs
  the volumetric raymarch with the 3D volume/noise/light textures; `--particles`
  draws additive billboards through the particle shader with a structured
  instance buffer; `--frame` composes sky → bloom → composite across render
  targets in one frame; `--scene-frame` renders sky, ground and the instanced
  scene into one shared HDR/depth target before bloom and composite; `--city`
  generates the procedural scene and renders its buildings, trees and vehicles
  from the shared mesh library, plus a volumetric smoke cloud, an additive
  particle burst and the shock-front refraction, through the same Vulkan frame.

All three are built by the `linux-core` CI job on Ubuntu 24.04; the job runs the
core tests and the Vulkan smoke test on Mesa lavapipe.

## Dependencies

Required now:

- CMake 3.21 or newer
- Ninja (or another CMake generator)
- A C++20 compiler (GCC 13 or Clang 17 are tested)
- Vulkan loader (`libvulkan-dev`) and, for headless CI, a software driver
  (`mesa-vulkan-drivers`)
- Desktop development headers for a windowed SDL3 build (`libx11-dev` and
  friends, or Wayland)
- Network access on the first configure for the Jolt, Vulkan-Headers and SDL3
  FetchContent downloads

Planned for the application build:

- DXC for HLSL to SPIR-V compilation

The `vulkan_shaders` CMake target compiles every real shader entry point to
SPIR-V with DXC. It is optional and only exists when `dxc` is found; CI installs
the pinned DXC release. Without X11 or Wayland headers SDL3 is configured in
console mode, so the offscreen Vulkan path still builds and runs; only window
creation is unavailable.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`EXPLOSION_BUILD_APP` defaults to `OFF` on Linux, so the configure step does not
try to build the Win32/D3D11 application. The option is accepted and reported
as not implemented yet.

## Vulkan smoke test

```sh
# Offscreen, any Vulkan device (uses lavapipe when forced):
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/vulkan_smoke --headless --width 320 --height 180 \
  --frames 2 --output /tmp/vulkan_smoke.ppm

# Windowed swapchain (needs a desktop session and SDL3 video drivers):
./build/vulkan_smoke --frames 120

# Vertex buffer + SPIR-V shaders + graphics pipeline:
./build/vulkan_smoke --headless --triangle --frames 2 \
  --output /tmp/vulkan_triangle.ppm

# Textured quad: texture, sampler, uniform buffer and descriptor set:
./build/vulkan_smoke --headless --quad --frames 2 \
  --output /tmp/vulkan_quad.ppm

# Real sky shader with the shared FrameConstants uniform buffer:
./build/vulkan_smoke --headless --sky --frames 2 \
  --output /tmp/vulkan_sky.ppm

# Real HDR composite (bloom + tonemap + vignette):
./build/vulkan_smoke --headless --composite --frames 2 \
  --output /tmp/vulkan_composite.ppm

# Ground plane with shadow maps and a 3D volume texture:
./build/vulkan_smoke --headless --ground --frames 2 \
  --output /tmp/vulkan_ground.ppm

# Render-to-texture: a quad into an offscreen target, then composite it:
./build/vulkan_smoke --headless --target --frames 2 \
  --output /tmp/vulkan_target.ppm

# Bloom: real downsample/upsample chain into render targets:
./build/vulkan_smoke --headless --bloom --frames 2 \
  --output /tmp/vulkan_bloom.ppm

# Scene shader: instanced cube with structured buffers and shadow maps:
./build/vulkan_smoke --headless --scene --frames 2 \
  --output /tmp/vulkan_scene.ppm

# Shock front: refraction pass with scene color and depth:
./build/vulkan_smoke --headless --shock --frames 2 \
  --output /tmp/vulkan_shock.ppm

# Volumetric raymarch with 3D volume, noise and light textures:
./build/vulkan_smoke --headless --volume --frames 2 \
  --output /tmp/vulkan_volume.ppm

# Additive particle billboards with a structured instance buffer:
./build/vulkan_smoke --headless --particles --frames 2 \
  --output /tmp/vulkan_particles.ppm

# Full frame: sky -> bloom -> composite across render targets:
./build/vulkan_smoke --headless --frame --frames 2 \
  --output /tmp/vulkan_frame.ppm

# Combined scene frame: sky + ground + scene in one HDR/depth target:
./build/vulkan_smoke --headless --scene-frame --frames 2 \
  --output /tmp/vulkan_scene_frame.ppm

# Procedural city: generated scene objects rendered as instanced cubes:
./build/vulkan_smoke --headless --city --frames 2 \
  --output /tmp/vulkan_city.ppm
```

The smoke shaders are compiled from `tools/vulkan_smoke/*.hlsl` and
`assets/shaders/sky.hlsl`/`composite.hlsl`/`ground.hlsl`/`bloom.hlsl`/
`scene.hlsl`/`shock.hlsl`/`volume.hlsl`/`particles.hlsl` with DXC and embedded
as SPIR-V in `TriangleShaders.h`, `QuadShaders.h`, `SkyShaders.h`,
`CompositeShaders.h`, `GroundShaders.h`, `BloomShaders.h`, `SceneShaders.h`,
`ShockShaders.h`, `VolumeShaders.h` and `ParticleShaders.h`.

Shared shaders use the `EX_VK_BINDING(n)` macro from `common.hlsli`. It expands
to `[[vk::binding(n, 0)]]` under DXC and to nothing under D3DCompile. The binding
map is `b0..b3 -> 0..3`, `t0..t7 -> 8..15`, `s0..s3 -> 16..19`; this avoids the
register-namespace collisions that DXC's default register mapping would create.

The smoke test prints the selected adapter and exits. `--debug` enables the
Vulkan validation layer when it is installed.

## WSL development flow

The repository is reachable from WSL through `/mnt/<drive>/...`:

```sh
cd /mnt/i/AI-coding/Explosion_Sim
cmake -S . -B ~/explosion-linux-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ~/explosion-linux-build --parallel
ctest --test-dir ~/explosion-linux-build --output-on-failure
```

Keep the build directory inside the Linux filesystem (`~/...`) rather than on
`/mnt/...`; the cross-filesystem I/O makes the build noticeably slower.

## DirectXMath on Linux

The scene and simulation types use `DirectX::XMFLOAT3`/`XMFLOAT4` and
`DirectXPackedVector` for half floats. Microsoft's DirectXMath headers are
vendored under `third_party/directxmath/Inc` (MIT). Outside Windows the build
adds `third_party/directxmath/compat` to the include path, which provides the
small `sal.h` annotation shim that the headers expect from the Windows SDK.

This keeps the math types and the existing code unchanged on both platforms.

## Platform notes

- `src/core/Log.h` guards the Win32 debug output and falls back to `stderr`.
- No `windows.h`, `ID3D11*` or `DXGI*` type is used by `explosion_core`.
- The renderer, window, splash and updater still depend on Win32/D3D11 and are
  ported in M2-M5.
