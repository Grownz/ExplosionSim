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
  (with a desktop) an SDL3 window with a swapchain.

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

Without X11 or Wayland headers SDL3 is configured in console mode, so the
offscreen Vulkan path still builds and runs; only window creation is unavailable.

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
