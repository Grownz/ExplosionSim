# Linux port and multi-platform RHI plan

Status: **M0 done, M1 nearly done, M2 started.** The portable simulation core
builds and passes `core_tests` natively on Linux (Ubuntu 24.04 CI job). The
backend-neutral RHI interfaces and the D3D11 backend exist; `GfxDevice` and the
swapchain run through the RHI. `SceneRenderer` creates its meshes,
instance/chunk/shadow buffers, shadow and surface-detail textures, shaders,
input layouts, fixed-function states and samplers through the RHI. The pass code
still binds resources through the temporary D3D11 context bridge.

M2 now has an SDL3 window layer and a Vulkan 1.2 device with an offscreen
target, swapchain, clear, present and readback (`vulkan_smoke`). The Vulkan
resource factories and the application integration are next; see
[`linux-build.md`](linux-build.md) for the current Linux build instructions.

## 1. Goal

Produce a **native Linux x64 build** of Explosion Simulator with feature parity to
the Windows build, and introduce a **render hardware interface (RHI)** that keeps the
simulation, scene and UI code independent of Direct3D 11 and Win32.

The Windows build must keep working throughout the migration. A native Linux build is
the primary goal; macOS is explicitly out of scope for the first parity milestone.

### Definition of done

- `cmake -S . -B build -DEXPLOSION_RHI=vulkan -DEXPLOSION_PLATFORM=sdl3` configures,
  builds and runs on Ubuntu 24.04 x64.
- The Linux build renders the full reduced-order scene, the high-fidelity GPU Euler
  solver, the volume bake, GPU particles, bloom, the shock-front refraction pass and
  the animated splash.
- The Windows build still uses the D3D11 backend and passes all existing tests.
- `physics` and `gpu` tests pass on both platforms; the GPU test is skipped only when
  no compatible device exists.
- CI builds both platforms on every push.
- No `ID3D11*`, `DXGI*`, `HWND` or `WIC` type appears outside the platform and
  D3D11 backend directories.
- A Linux package (AppImage or tar.gz) is produced and documented.
- Performance is within 10 % of the Windows build on the same GPU class at 1920x1080
  for the reference scenes.

## 2. Current state

### Portable code (reusable)

| Area | Files | Notes |
|---|---|---|
| Blast model | `src/sim/BlastModel.*` | Pure C++/math |
| Particles | `src/sim/ParticleSystem.*` | CPU and GPU state; GPU part needs RHI |
| Simulation | `src/sim/Simulation.*` | Thread pool, scene handoff |
| Scene generation | `src/scene/*` | Voronoi, physics (Jolt), scene objects |
| UI logic | `src/ui/Ui.cpp` | Dear ImGui draw code |
| Core | `src/core/*` | Thread pool, RNG, logging |
| Third party | Jolt, Dear ImGui core, miniz, nlohmann/json | All cross-platform |

### Platform-bound code

| Area | Files | Platform dependency |
|---|---|---|
| Device / swapchain | `src/gfx/GfxDevice.*` | D3D11, DXGI, HWND |
| Renderer | `src/gfx/Renderer.*` | 221 D3D11/DXGI references |
| Scene renderer | `src/gfx/SceneRenderer.*` | 149 references |
| Euler solver | `src/gfx/GpuEulerSolver.*` | 61 references, compute/UAV/indirect |
| Shaders | `assets/shaders/*.hlsl` | 18 files, ~3,000 lines HLSL |
| Shader compile | `src/gfx/Shader.*` | `D3DCompile` |
| Image I/O | `src/gfx/ImageIO.*` | WIC |
| Window / input | `src/app/App.cpp` | Win32 message loop, DWM, timers |
| Splash | `src/app/Splash.*` | GDI, WIC, Win32 |
| Update | `src/update/*` | WinHTTP, BCrypt, WinTrust, Authenticode |
| Build | `CMakeLists.txt` | Windows-only guard, `d3d11`, `dxgi`, `d3dcompiler`, `dwmapi`, `winmm`, `shell32`, `bcrypt` |

Measured size: `src/gfx` is ~5,600 lines, `src/app` ~2,100, `src/update` ~900.
The renderer and platform layers are the porting cost; the simulation and scene are
already largely portable.

## 3. Target architecture

```
                 +-------------------------------+
                 | App / UI (Dear ImGui)          |
                 | Simulation / Scene / Physics   |
                 +---------------+---------------+
                                 |
                 +---------------v---------------+
                 | Renderer (RHI only)           |
                 | passes, volume, particles     |
                 +---------------+---------------+
                                 |
                 +---------------v---------------+
                 | RHI (ex::rhi)                 |
                 | Device, Swapchain, Resources  |
                 | CommandList, Pipeline, Shader |
                 +---------+-------------+-------+
                           |             |
                 +---------v---+   +-----v---------+
                 | D3D11       |   | Vulkan        |
                 | Windows     |   | Linux/Windows |
                 +-------------+   +---------------+
                                 |
                 +---------------v---------------+
                 | Platform layer (ex::platform) |
                 | SDL3 window/input/DPI/files   |
                 | Win32 adapter on Windows      |
                 +-------------------------------+
```

### Backend choices

- **Windows:** D3D11 backend retained. It is the current, validated path and avoids a
  Windows regression during the Linux work.
- **Linux:** Vulkan 1.2+ backend. Compute shaders, storage buffers, indirect draws,
  3D textures and UAV-like storage images are required by the high-fidelity solver.
- **macOS (later):** Vulkan via MoltenVK, or a Metal backend if parity requires it.
  Not part of the first parity milestone.
- **OpenGL 4.5** is a fallback only. It is faster to prototype for the reduced-order
  path but weaker for the compute-heavy solver and harder to keep at parity.

### Platform layer

Use **SDL3** for window creation, input, DPI, clipboard, timers and file dialogs on
Linux and Windows. Dear ImGui has SDL3 + Vulkan/D3D11 backends. Win32-specific code
remains only in the Windows platform adapter and the updater.

Rationale: SDL3 covers the splash window, keyboard/mouse, high-DPI and file dialogs
with one dependency; GLFW would need separate solutions for the splash and dialogs.

## 4. RHI design

Namespace `ex::rhi`. The public API must not expose `ID3D11*`, `DXGI*`, `Vk*` or
`HWND`.

### Core objects

| Interface | Responsibility |
|---|---|
| `Device` | Create buffers, textures, samplers, shader modules, pipelines, command lists and fences; query capabilities; create swapchain |
| `Swapchain` | Acquire/present, resize, format and color space |
| `CommandList` | Begin/end render pass, set pipeline and resources, draw, dispatch, copy, barriers, viewport/scissor |
| `Buffer` | Vertex, index, uniform, storage, indirect, staging; mapped/staging helpers |
| `Texture` | 2D/3D, sampled, storage, render target, depth; mip levels and array layers |
| `ShaderModule` | Backend bytecode (DXBC or SPIR-V) plus reflection metadata |
| `Pipeline` | Shader stages, vertex layout, blend/depth/raster state, render targets, topology |
| `Fence` | CPU/GPU synchronization for staging readback and frame pacing |

### Conventions

- Clip space: define a single RHI convention and adapt in the Vulkan backend
  (Y flip, depth range 0..1 vs -1..1 as needed). Do not scatter the convention
  through renderer code.
- Texture origin, front-face winding and depth comparison are explicit pipeline
  fields, not implicit per backend.
- Resource lifetime is owned by `std::unique_ptr`/handle wrappers; no `ComPtr` in the
  public API.
- Capability queries: `supportsCompute`, `supportsIndirectDraw`, `supports3DStorage`,
  `maxTexture3DSize`, `supportsSplitVolumeFormats`, `supportsTimestampQueries`.

### Migration strategy (strangler, not big bang)

1. **Seam:** add `rhi::Device`/`rhi::Swapchain` and a D3D11 backend that wraps the
   existing `GfxDevice`. The renderer still receives a temporary legacy
   `ID3D11Device*` accessor from the D3D11 backend, so Windows keeps working.
2. **Resources:** port `SceneRenderer` buffers/textures/shaders/pipelines to the RHI.
3. **Passes:** port sky, ground, scene, volume, particles, shock, bloom, composite.
4. **Compute:** port `GpuEulerSolver`, `VolumeBaker` GPU path and GPU particles.
5. **Remove the legacy accessor** once no renderer code uses D3D11 types.
6. **Vulkan backend** implements the same interfaces; run the same tests and golden
   images.

Each step is a mergeable commit with the Windows build green.

## 5. Shader pipeline

- **D3D11:** keep DXBC via `D3DCompile` for the Windows backend during the migration.
- **Vulkan:** compile HLSL to SPIR-V with **DXC** (`-spirv -T <profile> -E <entry>`).
  Add explicit `[[vk::binding]]`/`[[vk::location]]` where reflection is insufficient.
- Build-time SPIR-V for release packages; optional runtime DXC for development.
- Introduce a shader manifest mapping entry points, stages, bytecode and reflection
  data. The RHI pipeline creation consumes the manifest.
- Add shader compile tests and a validation pass with the Vulkan validation layers.
- If a shader cannot be expressed cleanly in SPIR-V, keep a GLSL variant only for that
  shader and document the divergence.

## 6. Work packages

Estimates assume one experienced graphics engineer, the current codebase, and no
external art/asset work. They are person-days, not calendar promises.

### M0 — Plan, toolchain and Linux CI skeleton (3–5 days)

Status: **done.** The portable core and `core_tests` build on Ubuntu 24.04 in CI;
see [`linux-build.md`](linux-build.md).

- Approve this plan and the backend decisions.
- Add an Ubuntu 24.04 CI job with CMake, Ninja, GCC/Clang, Vulkan headers/loader,
  SDL3, DXC and Mesa (lavapipe for headless tests).
- Add CMake options and presets (`EXPLOSION_RHI`, `EXPLOSION_PLATFORM`).
- Document Linux build dependencies and the WSL development flow.
- Acceptance: CI configures a dummy Linux target and runs the portable physics tests.

### M1 — RHI core + D3D11 backend, Windows unchanged (10–15 days)

Status: **in progress.** RHI device/swapchain, buffers, textures, samplers,
shaders and pipelines are in place and `SceneRenderer` uses them. The temporary
D3D11 context bridge remains for the render passes and the compute path.

- Add `src/gfx/rhi/Rhi.h`, `rhi/d3d11/*`.
- Wrap `GfxDevice`/swapchain and port resource creation for the scene renderer.
- Keep a temporary legacy D3D11 accessor for unported code.
- Acceptance: Windows build and all tests pass; no visual or performance regression;
  D3D11 types confined to `rhi/d3d11` plus the temporary accessor.

### M2 — SDL3 platform layer + Vulkan device/swapchain (10–15 days)

Status: **in progress.** The SDL3 window/event layer, Vulkan instance, physical
and logical device, offscreen target, swapchain, clear, present and readback
exist and are exercised by `vulkan_smoke` on Windows and Linux. The application
still uses the Win32/D3D11 path.

- Add `src/platform/Platform.h` and `platform/sdl3/*`.
- Create window, input, DPI, timer, clipboard and file dialogs.
- Vulkan instance/device/swapchain, clear screen, resize, present.
- Add a Linux headless mode using an offscreen target (no window) for CI.
- Acceptance: Linux app opens a window, clears the screen and exits cleanly; resize and
  DPI work; CI produces a screenshot.

### M3 — Reduced-order renderer on Vulkan (25–35 days)

Status: **started.** Vulkan buffer, texture, sampler, shader-module, graphics
pipeline, descriptor-set and render-to-texture support exist. `vulkan_smoke`
runs the real sky, ground, composite, bloom, scene, shock, volume and particle
shaders plus a textured quad, triangle and a two-pass render-to-texture/
composite test. The scene test uses structured instance buffers and indexed
instanced draws; ground uses cascaded/static comparison-sampled shadow maps and
a 3D volume texture; bloom uses the real downsample/upsample chain; shock
samples scene color/depth; volume uses the 3D volume/noise/light textures with
premultiplied blending; particles use additive billboards and a structured
instance buffer. The Vulkan viewport flips Y so both backends share D3D's
screen-space convention. The general HLSL→SPIR-V build integration and the
application/renderer wiring are next.

- Port sky, ground, procedural scene, volume raymarch, bloom, composite, shock pass,
  particles and the shadow maps.
- Port the volume bake GPU path and the CPU upload path.
- Port `SceneRenderer` instancing and chunk geometry.
- Golden-image comparison against Windows with a tolerance.
- Acceptance: Linux renders the full reduced-order scene; `--screenshot` output is
  visually equivalent; 60 fps target at 1080p on the reference GPU.

### M4 — High-fidelity solver and GPU particles on Vulkan (25–40 days)

- Port `GpuEulerSolver`: state textures, sweeps, reduce/rescale, light volume, render
  volume, timestamp queries.
- Port the GPU particle spawn/integrate/finalize passes and indirect draws.
- Port the shock-front and volume shaders to SPIR-V.
- Acceptance: HF mode and solver benchmark run on Linux; solver stats and images match
  Windows within tolerance; validation layers clean.

### M5 — Splash, image I/O, settings, packaging (10–15 days)

- Replace GDI/WIC splash with SDL3 + SDL_image/SDL_ttf, keeping the 980:640 layout,
  20 frames and 5 % progress steps.
- Replace WIC with `stb_image`/`libpng` for PNG/JPEG loading on both platforms.
- Portable settings paths (`std::filesystem`, XDG on Linux, `%LOCALAPPDATA%` on Windows).
- Linux package: AppImage or tar.gz with assets; desktop entry and icon.
- Updater: Linux v1 uses GitHub releases and the package manager; the Windows
  self-updater stays behind a platform interface and is disabled on Linux.
- Acceptance: splash animation, settings round-trip and screenshot loading work on
  Linux; package runs from a clean directory.

### M6 — Parity, performance and hardening (10–20 days)

- Run the full test matrix on Windows and Linux, hardware and lavapipe.
- Profile and tune Vulkan barriers, descriptor updates and pipeline caches.
- Fix platform edge cases (multi-monitor DPI, alt-tab, suspend/resume, paths with
  non-ASCII characters).
- Update README, architecture and physics docs; add a Linux build section.
- Acceptance: definition of done above; release artifacts for both platforms.

### Dependency graph

```
M0 -> M1 -> M2 -> M3 -> M4 -> M5 -> M6
              \-> (Linux reduced-order milestone at end of M3)
```

A useful intermediate release is **M0–M3**: a native Linux build with the reduced-order
renderer, no high-fidelity solver and no self-updater.

## 7. Effort summary

| Scenario | Person-days | Calendar (1 dev) | Calendar (2 devs) |
|---|---:|---:|---:|
| Wine/Proton wrapper (no native binary) | 1–3 | 1–3 days | — |
| Minimal native Linux, reduced-order only (M0–M3) | 48–70 | ~2.5–3.5 months | ~1.5–2 months |
| Full parity (M0–M6) | 93–145 | ~4.5–7 months | ~2.5–4 months |

The range reflects unknowns in the Vulkan compute port, shader conversion and driver
differences. The high-fidelity solver is the largest single risk.

## 8. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| D3D11→Vulkan semantic gap (barriers, descriptors, coordinate conventions) | High | RHI conventions, validation layers, RenderDoc, incremental pass ports |
| HLSL→SPIR-V differences | High | DXC reflection, per-shader tests, GLSL fallback for problem shaders |
| Compute/UAV/indirect/3D texture support differences | High | Capability queries; reduced-order fallback; test on NVIDIA/AMD/Intel |
| Volume format support (RGBA16F, split RG8, 1024³) | Medium | Format queries, fallback to a supported layout, document limits |
| Performance parity | Medium | Benchmarks per pass, timestamp queries, pipeline cache, barrier tuning |
| Linux CI has no GPU | Medium | Mesa lavapipe for functional tests; hardware runs on a separate runner |
| Splash and updater platform code | Medium | SDL3 splash; drop the Linux self-updater in v1 |
| `wchar_t`/path handling | Low | `std::filesystem::path`, UTF-8 conversion at the platform boundary |
| Distribution (glibc, Vulkan loader) | Medium | AppImage/Flatpak; document minimum glibc and Vulkan versions |
| Scope creep (macOS, D3D12, new features) | High | Freeze feature work during the port; macOS after parity |

## 9. Testing and acceptance

- **Portable tests:** `physics` must pass on Linux unchanged.
- **GPU tests:** add a Vulkan variant of `gpu_tests`; run with validation layers on
  Linux and WARP/lavapipe in CI.
- **Golden images:** deterministic `--screenshot` captures for reduced-order and HF
  scenes; compare Windows vs Linux with a perceptual tolerance (SSIM or pixel delta).
- **Performance:** `--benchmark` and `--solver-benchmark` on the same GPU class;
  record per-pass timings and compare.
- **Platform:** window resize, DPI, input, clipboard, file paths with spaces/non-ASCII,
  missing assets, no Vulkan device fallback.
- **Regression:** Windows CI remains green after every milestone.

## 10. Open decisions

1. **Vulkan vs OpenGL** — recommendation: Vulkan for compute parity. OpenGL only if
   the reduced-order milestone must ship sooner and HF parity can wait.
2. **SDL3 vs GLFW** — recommendation: SDL3 for splash, input, DPI and dialogs.
3. **Linux updater** — recommendation: no self-updater in v1; GitHub releases plus
   AppImage/tar.gz.
4. **macOS** — out of scope for v1; revisit after Linux parity.
5. **Minimum Linux GPU** — Vulkan 1.2, compute, 2 GB VRAM recommended; document the
   reduced-order fallback for weaker devices.
6. **Shader compilation** — offline SPIR-V for release, runtime DXC for development.

## 11. First implementation step

When this plan is approved, start with **M0 + M1**:

1. Add the Linux CI/toolchain skeleton and CMake options.
2. Add `src/gfx/rhi/Rhi.h` and the D3D11 backend.
3. Port `GfxDevice`/swapchain and `SceneRenderer` resources behind the RHI while
   keeping the Windows build green.
4. Tag the result as an internal milestone; do not publish a Linux binary yet.

This keeps the Windows product stable and creates the seam needed for the Vulkan
backend without a risky big-bang rewrite.
