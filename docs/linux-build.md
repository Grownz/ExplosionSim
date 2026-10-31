# Linux build

The portable simulation core, the Vulkan bring-up target and the native Linux
application shell build and run on Linux. The shell owns the SDL3 window and the
Vulkan RHI device, resizes the swapchain and writes offscreen PNG/PPM captures;
the reduced-order renderer and the high-fidelity GPU Euler solver are ported
into it (milestones M2-M4 in [`linux-port.md`](linux-port.md)). For
installation, running, settings and troubleshooting, see
[`linux-usage.md`](linux-usage.md).

## Current Linux targets

- `explosion_core`: blast model, particles, simulation, CPU volume bake, scene
  generation, Voronoi fracture, Jolt physics, thread pool.
- `core_tests`: portable smoke tests for the blast model, deterministic scene
  generation and the CPU volume bake.
- `physics_tests`: the shared physics suite (blast model, volume baker, scene,
  Voronoi fracture, Jolt physics, thread pool) on Linux. The settings,
  changelog, version and SHA-256 sections are Windows-only in the source
  because the settings serializer pulls in the Win32 renderer.
- `gpu_vulkan_tests`: offscreen Vulkan variant of the GPU solver test. It runs
  the RHI Euler solver through reset, steps, render/light volumes, timestamps
  and rescale; it is skipped when no Vulkan device exists.
- `explosion_linux`: native SDL3 + Vulkan application shell. It drives
  `sim::Simulation`, frames the blast with `gfx::Camera` and renders the
  reduced-order chain (sky, ground, procedural scene, volume bake, particles,
  shock, bloom and composite) through the RHI. `--high-fidelity` switches the
  volume source to the GPU Euler solver (state sweeps, reduction, render and
  light volumes). Windowed runs show an ImGui settings/controls panel (F1). It
  shows the SDL3 splash in windowed mode and can write an offscreen
  `--screenshot` as PNG or PPM.
- `package_linux`: builds a self-contained `ExplosionSim-linux-x86_64.tar.gz`
  with the binary, assets, desktop entry, icon and Linux README.
- `vulkan_smoke`: Vulkan 1.2 device, offscreen target, clear, readback and
  (with a desktop) an SDL3 window with a swapchain. `--triangle` exercises
  vertex buffers, SPIR-V shader modules and a graphics pipeline; `--quad` adds
  a texture, sampler, uniform buffer and descriptor set; `--sky` runs the real
  `assets/shaders/sky.hlsl` with the shared frame constants; `--composite` runs
  the real tonemap/bloom composite with two sampled textures; `--ground` runs
  the ground plane with cascaded/static shadow maps and a 3D volume texture;
  `--target` renders a pass into an offscreen texture through `rhi::CommandList`
  and samples it afterwards;
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

All four are built by the `linux-core` CI job on Ubuntu 24.04; the job runs the
core tests, the Vulkan smoke test and the Linux application screenshot on Mesa
lavapipe.

The D3D11 application now records its scene shadow and main passes through
`rhi::CommandList`; `Renderer` owns its HDR/bloom targets as RHI textures. The
Vulkan backend implements the same command-list interface on top of the current
single-frame command buffer, so the remaining renderer passes can move over
without changing their logic.

## Dependencies

Required now:

- CMake 3.21 or newer
- Ninja (or another CMake generator)
- A C++20 compiler (GCC 13 or Clang 17 are tested)
- Vulkan loader (`libvulkan-dev`) and, for headless CI, a software driver
  (`mesa-vulkan-drivers`)
- Desktop development headers for a windowed SDL3 build (`libx11-dev` and
  friends, or Wayland)
- Network access on the first configure for the Jolt, Vulkan-Headers, SDL3 and
  stb FetchContent downloads

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

The repository also ships a `linux-x64` configure preset and `linux-release`
build/test presets, which mirror the Windows `windows-x64`/`release` presets
and build into `build-linux`:

```sh
cmake --preset linux-x64
cmake --build --preset linux-release
ctest --preset linux-release
```

`EXPLOSION_BUILD_APP` defaults to `OFF` on Linux, so the configure step does not
try to build the Win32/D3D11 application. The native Linux shell is built as the
separate `explosion_linux` target whenever Vulkan and SDL3 are available.

`ctest` runs five suites on Linux: `core`, `splash`, `updater`, `physics` and
`gpu_vulkan`. On Windows the suites are `core`, `splash`, `physics` and `gpu`
(D3D11). The GPU suite is skipped only when no compatible device exists.

### Benchmarks and hardening

```sh
# Frame timing (render-only, deterministic scene):
./build/explosion_linux --settings tools/capture_settings.ini \
  --benchmark --frames 60 --width 320 --height 180

# Solver-only timing with per-pass GPU timestamps:
./build/explosion_linux --settings tools/capture_settings.ini \
  --solver-benchmark --solver-resolution 32 --no-solver-auto-scale \
  --solver-step-count 64 --solver-steps 16
```

The benchmark prints `Benchmark: ...` and `Average: ... ms/frame (fps)`.
Lavapipe at 320x180 measures about 18 ms/frame for the reduced-order capture
profile; the solver benchmark reports the GPU total plus the X/Y/Z sweep,
reduce and render passes. These are functional baselines, not a hardware
parity claim: the 10 % same-GPU-class target needs a hardware run.

Hardening in M6:

- Minimized windows stop rendering and keep the frame clock in sync.
- The swapchain uses `SDL_GetWindowSizeInPixels`, so high-DPI windows render at
  the pixel resolution.
- Settings, asset and screenshot paths use `std::filesystem`/UTF-8; CI renders
  from a non-ASCII settings path.
- Volume texture descriptor rewrites are cached until the texture changes, and
  the Vulkan device uses a pipeline cache.
- The volume, particle and shock passes sample the real scene depth (transitioned
  to a depth-read layout after the scene pass) and use a dummy depth attachment,
  so smoke and effects are occluded by buildings. The ground pass keeps the far
  proxy because it renders inside the main depth pass.
- CI runs the Vulkan validation layers on lavapipe for both the reduced-order
  and high-fidelity paths, and a separate ASan/UBSan job builds and runs the
  core and physics tests.
- A tag-triggered `release` job attaches the Windows zip and Linux tar.gz to a
  GitHub release.

## Linux application shell

```sh
# Offscreen PNG capture through the Vulkan RHI:
./build/explosion_linux --headless --width 1600 --height 900 \
  --frames 2 --screenshot /tmp/explosion_linux.png

# Interactive window (needs a desktop session and SDL3 video drivers):
./build/explosion_linux
```

The shell runs `sim::Simulation` and renders the full reduced-order preview
through the Vulkan RHI: sky, ground, the generated town, the volume bake,
the additive/fire/alpha particle batches, the shock-front refraction, bloom and
the tonemap composite. The volume bake and particle integration run as compute
shaders by default; `--no-gpu-volume-bake` and `--no-gpu-particles` force the
CPU paths. The camera follows the same reduced-order framing as the Windows
app. `--screenshot` accepts `.png` and `.ppm`; the scene seed, density, charge,
burst height, volume resolution and the main render settings can be overridden
from the command line for capture comparisons. The shell loads the
same `settings.ini` keys as the Windows app and renders the Voronoi chunk
geometry for collapsed buildings. `tools/capture_settings.ini` is the shared
deterministic capture profile; pass `--settings <path>` to load any compatible
file.

### High-fidelity solver

`--high-fidelity` replaces the analytic volume bake with the RHI port of the
compressible Euler solver (`src/gfx/RhiEulerSolver.*`, SPIR-V in
`src/app/Euler*Shader.h`). It runs the init, X/Y/Z sweeps (with the fused
source), reduction, render-volume, light-volume and rescale passes, and feeds
the render and light volumes into the volume raymarcher. The solver is
deterministic in headless captures; `--solver-resolution`, `--solver-steps`,
`--solver-time` and `--no-solver-auto-scale` mirror the Windows flags.

```sh
# Deterministic high-fidelity capture (32^3 grid, 0.01 s):
./build/explosion_linux --settings tools/capture_settings.ini \
  --high-fidelity --solver-resolution 32 --no-solver-auto-scale \
  --headless --width 320 --height 180 --frames 1 --time 0.01 \
  --charge 0.2 --distance-scale 0.55 \
  --screenshot /tmp/explosion_linux_hf.png
```

The Windows counterpart uses the same settings and flags. At 32^3 and 0.01 s
both report the same step count, time step and maximum speed (80 steps,
~236 us, ~718 m/s). `tools/reference/win_hf_capture.png` is the checked-in
Windows reference; the CI `capture-parity` job enforces a 22/255 mean and 30 %
pixel limit. The wider window than the reduced-order capture covers the
remaining preview-renderer gap (the solver volume itself matches in position
and density).

`--solver-benchmark` runs the solver for a fixed number of steps and reports
the GPU timestamps per pass, mirroring the Windows benchmark:

```sh
./build/explosion_linux --settings tools/capture_settings.ini \
  --solver-benchmark --solver-resolution 32 --no-solver-auto-scale \
  --solver-step-count 64 --solver-steps 16
```

The output includes the GPU total, the three sweeps, the reduction and the
render-volume pass. Timestamps are read once after all steps are submitted, so
the benchmark itself adds no per-step synchronisation.

### Splash, settings and packaging

Windowed runs show the SDL3 splash while the device, pipelines and scene are
prepared. It uses the same 980x640 layout and 20 stills as the Windows splash,
one per 5 % progress step. The frames are decoded by the portable stb image
loader; no WIC or SDL_image is required. Linux pumps the splash cooperatively
between initialisation stages; the Windows application runs the same SDL3
splash on a worker thread so the animation continues during shader compilation.

The text is rasterized from the same TTF family as the ImGui panel with
stb_truetype, so the splash matches the rest of the program: Segoe UI on
Windows, and DejaVu Sans, Liberation Sans or Noto Sans on Linux. If no TTF is
found, the splash falls back to SDL's bitmap font. Consecutive stills crossfade
instead of snapping; the fade lasts 60 % of the measured time between two
images, clamped to 120-1000 ms, so it follows the loading speed.

The splash can be exercised without a display:

```sh
SDL_VIDEODRIVER=dummy ./build/explosion_linux --splash-test \
  --splash-screenshot /tmp/splash.png
```

`ctest` also runs the `splash` suite, which renders the splash with the dummy
video driver, checks that the title uses the shared TTF and samples a frame in
the middle of a crossfade. It runs on Windows and Linux.

### Update helper

`explosion_updater` is built next to the application and packaged as
`ExplosionSimUpdater`. It owns the GitHub API check, the asset download, the
SHA-256 verification, the tar.gz extraction and the install swap, and it
relaunches the application after the swap. HTTP/1.1 and TLS are implemented in
the helper on top of a vendored mbedTLS, so the application keeps no network
dependency. The helper only updates a writable per-user installation.

`ctest` runs the `updater` suite, which covers SHA-256, URL validation, tar.gz
extraction and the install swap including the relaunch of a stub executable.

Settings are resolved in this order: `settings.ini` in the working directory,
next to the executable, then the per-user path
(`$XDG_CONFIG_HOME/explosionsim/settings.ini`, falling back to
`~/.config/explosionsim/settings.ini`). The Windows app uses
`%LOCALAPPDATA%\ExplosionSim\settings.ini` as its fallback. `--write-settings`
dumps the active profile and exits; reloading it reproduces the capture exactly:

```sh
./build/explosion_linux --settings tools/capture_settings.ini \
  --write-settings /tmp/roundtrip.ini
./build/explosion_linux --settings /tmp/roundtrip.ini --headless \
  --width 320 --height 180 --frames 1 --time 0.1 --charge 0.2 \
  --distance-scale 0.55 --screenshot /tmp/roundtrip.png
```

Build the Linux package with:

```sh
cmake --build build --target package_linux
```

This writes `build/ExplosionSim-linux-x86_64.tar.gz`. Extracting it and running
`./ExplosionSim` from the extracted directory is a supported clean-directory
install; the assets, `ExplosionSimUpdater` and
`explosionsim.desktop`/`explosionsim.png` are included. The application updates
a per-user install in place through `ExplosionSimUpdater`; see
[`linux-usage.md`](linux-usage.md#updating).

For a capture comparison, render the same settings on both platforms and run:

```sh
python3 tools/compare_png.py windows.png linux.png --tolerance 20 \
  --max-fraction 0.20
```

The current reference capture is at about 13/255 mean absolute RGB difference
and 16.5 % of pixels above the 32/255 threshold. `tools/reference/win_capture.png`
is the checked-in Windows reference (RTX 3090, 320x180, `capture_settings.ini`);
the CI `capture-parity` job enforces the 20/255 mean and 20 % limits against it.

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
The Euler solver uses its own layout (`b0 -> 0`, `t0/t1 -> 8/9`, `s0 -> 16`,
`u0/u1 -> 21/22`, reduce output `-> 23`, render output `-> 24`, light input
`-> 10`, light output `-> 25`) so the same image can be a sampled or storage
descriptor without register aliasing. The solid obstacle mask is `t3 -> 11` in
the solver passes and `t0 -> 22` in the reduced-order volume bake, which keeps the
bake output at `u0 -> 21` unchanged.

The compute shaders that the Linux application embeds (the Euler passes and the
volume bake) are checked-in headers under `src/app`. After editing their HLSL,
regenerate the matching header with the same DXC flags the build uses:

```sh
mkdir -p build/spirv
dxc -T cs_6_0 -E CSBake -spirv -fvk-use-dx-layout -DEXPLOSION_VULKAN \
  -I assets/shaders -Fo build/spirv/volume_bake_CSBake.spv \
  assets/shaders/volume_bake.hlsl
python3 tools/embed_spirv.py build/spirv/volume_bake_CSBake.spv \
  src/app/VolumeBakeShader.h kVolumeBakeCS
```

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
