# Linux usage

This guide covers the native Linux build (`explosion_linux` / the packaged
`ExplosionSim`). For building from source, see
[linux-build.md](linux-build.md); for the port plan and status, see
[linux-port.md](linux-port.md).

## Install

The release archive is self-contained:

```sh
tar xzf ExplosionSim-linux-x86_64.tar.gz
cd ExplosionSim-linux-x86_64
./ExplosionSim
```

To install into the per-user XDG directories (binary, assets, desktop entry and
icon):

```sh
bash install.sh
```

The package is built on Ubuntu 24.04 and needs **glibc 2.39 or newer**. A Vulkan
1.2 capable driver is required; the renderer uses compute, 3D storage images and
indirect draws. `vulkaninfo --summary` should list a device.

## Running

```sh
# Interactive window:
./ExplosionSim

# Headless reduced-order capture:
./ExplosionSim --headless --width 1600 --height 900 --frames 1 \
    --time 0.12 --screenshot capture.png

# High-fidelity Euler solver capture:
./ExplosionSim --high-fidelity --solver-resolution 32 \
    --no-solver-auto-scale --headless --width 320 --height 180 \
    --frames 1 --time 0.01 --charge 0.2 --distance-scale 0.55 \
    --screenshot hf.png
```

Useful flags:

| Flag | Meaning |
|---|---|
| `--settings <path>` | Load a compatible `settings.ini` profile. |
| `--write-settings <path>` | Dump the active profile and exit. |
| `--no-gpu-volume-bake` | Use the CPU volume baker. |
| `--no-gpu-particles` | Use the CPU particle integration. |
| `--high-fidelity` | Use the GPU Euler solver. |
| `--solver-resolution <n>` | Solver grid resolution (32-512). |
| `--solver-steps <n>` | Maximum solver steps per frame. |
| `--solver-time <s>` | Solver time for a high-fidelity capture. |
| `--no-solver-auto-scale` | Keep the solver domain fixed. |
| `--flow-around` / `--no-flow-around` | Include the scene buildings and vehicles as solid obstacles in the high-fidelity solver (default on). |
| `--body-drag` / `--no-body-drag` | Apply the solver's sampled flow as drag impulses to the debris bodies (default on). |
| `--benchmark --frames <n>` | Render a fixed frame count and report the average. |
| `--solver-benchmark` | Report solver GPU timings per pass. |
| `--splash-test` | Render the splash briefly and exit. |
| `--debug` | Enable Vulkan validation layers when installed. |

## User interface

Windowed runs show an ImGui panel (F1 toggles it). It provides:

- pause, detonate, reset and frame-camera buttons;
- charge mass, height of burst and time scale, plus a high-fidelity toggle and
  solver grid;
- exposure, bloom, raymarch steps and volume/particle/ground/bloom/grid toggles.

"Apply blast settings" restarts the blast with the edited values; the render
controls apply immediately. The panel needs a desktop session; headless
captures, benchmarks and the splash test do not initialize it.

## Settings

The app looks for `settings.ini` in this order:

1. the working directory,
2. the directory containing the executable,
3. `$XDG_CONFIG_HOME/explosionsim/settings.ini` (falling back to
   `~/.config/explosionsim/settings.ini`).

The file uses the same keys as the Windows application
(`[simulation]`/`[render]`). `--write-settings` writes an exact round-trip of
the active profile. The shared deterministic capture profile is
`tools/capture_settings.ini`.

## Benchmarks

```sh
# Frame timing (render-only, deterministic scene):
./ExplosionSim --settings tools/capture_settings.ini \
    --benchmark --frames 60 --width 320 --height 180

# Solver-only timing with per-pass GPU timestamps:
./ExplosionSim --settings tools/capture_settings.ini \
    --solver-benchmark --solver-resolution 32 --no-solver-auto-scale \
    --solver-step-count 64 --solver-steps 16
```

The frame benchmark prints `Average: ... ms/frame (fps)`. The solver benchmark
prints the GPU total plus the X/Y/Z sweep, reduce and render passes. Lavapipe
(software Vulkan) is useful for functional CI, but its numbers are not a
hardware performance claim; the 10 % same-GPU-class comparison requires a
native install with a real GPU driver.

## Troubleshooting

- **No Vulkan device**: install the vendor driver (`mesa-vulkan-drivers` on
  Mesa, the NVIDIA/AMD driver package otherwise) and check `vulkaninfo`.
- **`vkCreateDevice`/surface failure**: the driver may be older than Vulkan
  1.2, or the window system integration extension is missing.
- **Missing splash or models**: keep the `assets` directory next to the
  executable. Shaders are embedded, but the splash frames and scene models are
  not.
- **glibc mismatch**: the release archive needs glibc 2.39+. Rebuild from
  source on an older distribution or use a newer one.
- **Validation layers**: install `vulkan-validationlayers` and run with
  `--debug`; errors are logged as `[error] Vulkan: ...`.
- **High-DPI / multi-monitor**: the swapchain uses the window's pixel size.
  If a compositor reports an unexpected scale, check the SDL video driver and
  `SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR`.
- **Minimized window**: rendering pauses while minimized and resumes on
  restore.

## Updating

The application checks the GitHub releases API once per day (and on demand from
the Updates section of the panel). When a newer release is available it can
download `ExplosionSim-linux-x86_64.tar.gz`, verify the SHA-256 digest published
by GitHub, install it and restart itself.

The install step is performed by `ExplosionSimUpdater`, a helper binary shipped
next to the application. It waits for the application to exit, extracts the
archive, moves the current install directory aside, swaps in the new one and
relaunches the application; on failure it restores the previous directory. A
portable `settings.ini` inside the install directory is preserved, and the XDG
settings are never touched.

The updater only manages a per-user installation (`~/.local/share/ExplosionSim`
as created by `install.sh`). If the application is installed system-wide, run
the update through the package manager instead; the in-app install will fail
because the directory is not writable.

`--no-update-check` disables the startup check for one run. The check can also
be disabled in the panel; the skipped version and the last check time are stored
in `settings.ini`.

To update manually, download the newer archive, extract it and re-run
`install.sh` (or replace the directory). Settings in the XDG directory are
preserved.

## Tests

The Linux CI builds and runs:

- `core_tests` - portable core smoke tests,
- `physics_tests` - shared physics/scene/volume/Jolt suite,
- `gpu_vulkan_tests` - offscreen Vulkan solver test (skipped without a device),
- `vulkan_smoke` - backend bring-up passes,
- the reduced-order and high-fidelity screenshot parity checks,
- the settings round-trip, splash, package and validation-layer checks.

Run them locally with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
