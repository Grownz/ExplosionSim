# Linux build

The portable simulation core already builds and passes its tests natively on
Linux. The graphical application is still Windows-only until the SDL3 platform
layer and the Vulkan backend land (milestones M2/M3 in
[`linux-port.md`](linux-port.md)).

## Current Linux target

- `explosion_core`: blast model, particles, simulation, CPU volume bake, scene
  generation, Voronoi fracture, Jolt physics, thread pool.
- `core_tests`: portable smoke tests for the blast model, deterministic scene
  generation and the CPU volume bake.

Both are built by the `linux-core` CI job on Ubuntu 24.04.

## Dependencies

Required now:

- CMake 3.21 or newer
- Ninja (or another CMake generator)
- A C++20 compiler (GCC 13 or Clang 17 are tested)
- Network access on the first configure for the Jolt FetchContent download

Planned for the application build:

- Vulkan 1.2 loader and headers (`libvulkan-dev`)
- SDL3 development files
- DXC for HLSL to SPIR-V compilation

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`EXPLOSION_BUILD_APP` defaults to `OFF` on Linux, so the configure step does not
try to build the Win32/D3D11 application. The option is accepted and reported
as not implemented yet.

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
