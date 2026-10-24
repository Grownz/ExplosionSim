# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Absorbing sponge over the outer 6 % of the lateral and top solver boundaries. It
  relaxes the state towards ambient so outgoing acoustic waves do not reflect back into
  the plume; the ground remains a reflecting wall.
- Deterministic readback mode for headless captures: the solver waits for the reduction
  every 8 steps, so repeated captures produce identical images.

### Changed

- Solver CFL reduced from 0.30 to 0.20 and time steps are advanced in sub-batches of
  8 steps. Interactive runs poll the reduction through an event query without blocking;
  a rising maximum signal speed is picked up between sub-batches instead of after a
  whole batch.
- High-fidelity time captures now advance the analytic model together with the solver,
  so ground scorch, dust and the frame camera use the correct simulation time.

### Fixed

- High-fidelity `--time` captures advanced the analytic blast model twice (once in
  `RunHeadless` and once through the solver advance).
- A stale bloom texture was still added to the composite after bloom was disabled.
- The high-fidelity frame camera now includes the wind drift applied to the rendered
  volume instead of framing the origin.

### Known issues

- The auto-scale radial trigger reacts to the low base surge. On large surface bursts
  this can expand the domain earlier than the visible cloud needs, which coarsens the
  plume. A height-gated radial measurement is the next step.

## [0.2.0] - 2026-10-24

### Added

- **High-fidelity GPU mode**: a 3D compressible Euler solver (HLLC Riemann solver,
  MUSCL/minmod reconstruction, CFL-adaptive time step, buoyancy and soot transport)
  running in Direct3D 11 compute shaders at 64³–512³. Selectable under
  *Physics & Effects → Simulation model*; the solver field drives the existing
  volumetric renderer. 1024³ is not offered because its four RGBA32F state volumes would
  need about 76 GB of VRAM.
- **Solver auto-scale**: a GPU reduction measures the visible product bounds every step.
  When the plume top or radius reaches 75 % of the domain, the domain grows by 1.3× and
  the six conserved variables are resampled trilinearly onto the new grid (fixed voxel
  count). Several expansions can run in one frame, so a fast plume rise is caught before
  it clips; the accuracy budget is ×16. The UI shows the domain, voxel size, measured
  plume top/radius, accuracy factor and expansion count, and `--solver-auto-scale` /
  `--no-solver-auto-scale` are available.
- High-fidelity look development: product incandescence cools with the simulation time
  and fades with altitude, scattering and emission are tuned independently
  (`VolumeParams2`), gradient-based sun shading adds volumetric structure to the
  raymarch, and the lowest voxel layer is faded so the ground-reflected base surge no
  longer forms a hard star.
- Auto-scale volume box in the Rendering panel: it always contains the fireball, smoke
  envelope, cloud cap and base dust, grows only when the field approaches the edge and
  keeps the bake cost constant by trading effective resolution for size.
- Volume resolutions 256³, 512³ and 1024³. 256³/512³ use RGBA16F; 1024³ is split into
  two RG8 planes to stay within the Direct3D 11 4 GiB resource limit.
- Tileable 3D noise texture with domain warping and fine density modulation for sharper
  fireball, smoke and dust detail.
- Physically active atmosphere: moist-air density (Magnus-Tetens), humidity-dependent
  sound speed, Sachs pressure scaling for the blast and fireball, plus humidity controls
  and derived readouts in the Atmosphere panel.
- Adaptive metre scale bar in the lower-left corner when the grid overlay is enabled.
  The bar length is snapped to a 1-2-5 sequence and clamped to a readable pixel size as
  the camera zooms.
- Headless high-fidelity captures can target an exact solver step count
  (`--solver-step-count`); headless runs now load `settings.ini`, and `--reduced-order`
  forces the analytic model when the settings select the Euler solver.
- The README now includes a high-fidelity mushroom-cloud screenshot captured at solver
  step 35 000.

### Changed

- Default playback no longer loops after the blast; the grid overlay (including the
  metre scale) is enabled by default.
- Fireball rendering overhaul: hot filamentary core, cooler soot-rich surface, stronger
  Planckian emission, longer incandescence, and a growing soot shell.
- Dust and smoke now form a turbulent envelope and a shock-lifted base surge that
  progressively obscure the cooling fireball; particles were darkened and enlarged.
- Default volume resolution raised to 128³ and ray-march steps to 128.
- The high-fidelity solver now offers 192³, 256³ and 512³ grids (with a VRAM guard and
  non-real-time warning), uses the moist atmosphere and wind drift, and shares the
  domain-warped detail noise with the reduced-order volume.
- The hero screenshot was refreshed for the new rendering.

### Fixed

- The initial fireball no longer follows the axis-aligned noise lattice: the lump noise
  uses a fixed world-space frequency and its amplitude fades in with the fireball
  radius, so the first milliseconds stay spherical. The domain warp is faded out for
  very small fireballs as well.
- Duplicate Dear ImGui widget IDs for the `Pause` button/checkbox and the `Bloom`
  slider/checkbox, which triggered the "visible items with conflicting ID" warning.

## [0.1.0] - 2026-10-24

First public version.

### Added

- Physically based reduced-order blast model: Brode overpressure, Rankine-Hugoniot
  shock speed with adaptive front integration, Sedov-like fireball growth, buoyant
  mushroom cloud, thermal radiation and Friedlander observer waveform.
- Direct3D 11 renderer with volumetric ray marching, blackbody emission, instanced
  particles, procedural sky/terrain, HDR bloom and ACES tonemapping.
- Multithreaded particle integration and volume baking using a work-stealing pool.
- Dear ImGui control panel with live physical readouts and damage radii.
- Reference scenario: 200 g TNT, 0.4 m height of burst, 2 s fuse, slow-motion playback.
- Deterministic headless screenshot mode and an offscreen benchmark mode.
- Physics regression tests, CMake presets and a Windows CI workflow.

[0.2.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.2.0
[0.1.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.1.0
