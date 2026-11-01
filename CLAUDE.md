# Claude Code - Explosion Simulator

@AGENTS.md

The working agreement above is binding: the cross-platform rule, the required
verification, the platform seams, the conventions and the release/reference
workflows. This file only adds how to work in this checkout.

## Definition of done

A change is finished when all of the following hold, not when it compiles:

1. Windows configure/build/`ctest` and Linux configure/build/`ctest` are green
   (Linux via WSL is fine).
2. Rendering changes show the documented capture parity; simulation/physics
   changes also pass the sanitizer suite.
3. The user-visible behaviour is documented (`CHANGELOG.md` under
   `[Unreleased]`, and `docs/` where a model or workflow changed).
4. Both platforms are really covered - no silent one-platform change. If one
   side cannot be verified, report the blocker in the three-part form from
   `AGENTS.md`.

## Working in this checkout

- `build/Release/settings.ini` is a live user profile. Back it up before capture
  runs (they copy `tools/capture_settings.ini` over it) and restore it
  afterwards.
- The checked-in reference images are deliberately bit-stable. Regenerate them
  only when a rendering change is intended, and update the parity numbers in
  `docs/linux-port.md` in the same change.
- Keep diffs focused: match the surrounding style, do not reformat untouched
  code, and keep new sources in the existing `CMakeLists.txt` lists.
- Prefer the existing seams over new `#ifdef`s: `src/gfx/rhi/Rhi.h` for
  rendering, `src/platform/Platform.h` for windows and paths.

## Fast entry points

| Topic | Start here |
|---|---|
| Settings keys (parse/save/UI) | `src/app/Settings.cpp`, `src/ui/Ui.cpp`, `src/app/LinuxUi.cpp` |
| Blast and scene physics | `src/sim/BlastModel.*`, `src/scene/Scene.cpp`, `docs/physics.md` |
| Vehicle data | `src/scene/VehicleModels.h` |
| High-fidelity solver | `src/gfx/GpuEulerSolver.*`, `src/gfx/RhiEulerSolver.*`, `assets/shaders/euler_*` |
| 4C drag coupling | `src/sim/FlowBodies.*`, `Simulation::CollectFlowBodies`, `docs/physics.md` |
| Windows app loop | `src/app/App.cpp`, `src/gfx/Renderer.cpp` |
| Linux app loop | `src/app/LinuxApp.cpp`, `src/app/LinuxPreview.cpp` |
| Tests | `tests/physics_tests.cpp` (most behaviour), `tests/core_tests.cpp`, `tests/gpu*_tests.cpp` |
| CI | `.github/workflows/build.yml` |

## Verification shortcuts

```powershell
# Windows
cmake --build --preset release; ctest --preset release
```

```sh
# Linux (WSL), from /mnt/i/AI-coding/Explosion_Sim
cmake --build --preset linux-release && ctest --preset linux-release
```

When only documentation, comments or the changelog changed, say so explicitly
and skip the heavy matrix instead of pretending it ran.
