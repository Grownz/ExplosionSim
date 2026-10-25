# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Progressive building collapse: chunks form a six-neighbour support graph with per-edge
  joints and staggered failure times. Every failure re-runs the support flood fill, so
  chunks detach in waves instead of all at once; strongly loaded joints fail immediately,
  the rest within a few tenths of a second.
- Static structure colliders: chunks that are still standing become static boxes, so
  falling debris piles up against them. Detached chunks reuse the same body and are
  switched to dynamic, and a contact impulse above 500 N s damages the struck chunk's
  joints, which can start a local cascade.
- Optional Voronoi rubble (roadmap 2.3 phase B, *Environment > Voronoi chunks*,
  `scene.voronoiChunks`, default off): each building is split into convex cells
  clipped to its box. The cells keep flat-shaded hulls for rendering and a
  principal-component fitted OBB as the collision proxy, and shared faces become the
  joints of the existing support graph. Hulls are drawn with one non-instanced draw call
  per building through a per-vertex chunk id.
- Finer, irregular chunking: the default grid is now 2x3x2, `scene.quality = 1` uses
  3x4x3 and `quality = 2` 3x5x3, the split planes jitter by +-28 % per axis with a
  minimum chunk width of 0.28 cell, and the chunk mesh is a chamfered box (bevel 0.10 of
  the half extent). Rubble loses most of its right angles while the physics body stays an
  OBB.
- Building chunks keep the facade of the part they broke off from: every chunk instance
  carries a texture origin and scale that maps its mesh-local position back to the
  original building space, and blast damage is drawn as a procedural char and crack
  overlay on top of the intact facade instead of a flat recolour.
- High-fidelity mode follows the playback time-scale slider: the configured
  `solverStepsPerFrame` is the budget at the default 0.06 and is scaled by
  `timeScale / 0.06` (clamped to 1..96 steps), so slower playback resolves the same
  simulated interval with fewer, faster steps and faster playback with more.
- Blast shielding with nine rays per receiver: each ray accumulates the optical depth of
  every building it crosses (`exp(-depth / 3 m)`), a diffraction floor keeps shadowed
  receivers from going to zero, and an optional `sceneGroundReflection` setting adds a
  height-dependent surface reflection.
- Cascaded directional shadows: three cascades (2048/1024/1024) with practical splits,
  slope-scaled normal offset and 5x5 PCF. The far cascade draws only the large silhouettes,
  trees switch to a single-cone mesh beyond 80 m, and the ground samples the same cascades.
- Camera-frustum culling for intact objects and detached chunks, a distance-based fade
  of the procedural facade and sub-pixel culling of distant rubble chunks.
- Density scaling beyond the object budget: `scene.density` also packs `ceil(density)`
  smaller objects into each city block.
- Scene generation and benchmark logs report building/vehicle/tree, chunk and rigid-body
  counts, plus the number of awake bodies.
- `--distance-scale` now also applies to the reduced-order camera framing.

### Changed

- **Rigid-body backend replaced by Jolt Physics 5.6.0 (MIT).** The in-house solver
  (uniform grid, SAT, sequential impulses, shock propagation) is gone; Jolt provides the
  BVH broadphase, persistent manifolds with warm starting, box inertia, island sleeping
  and a multithreaded solver. The `PhysicsWorld` interface is unchanged, body state is
  mirrored for the renderer and tests, contact impulses are estimated with
  `EstimateCollisionResponse`, and the left-handed scene is mirrored on Z at the boundary.
  At density 3 the 1920x1080 benchmark dropped from 8.0 to 5.75 ms/frame (Voronoi) and
  from 8.7 to 4.16 ms/frame (grid). Jolt is fetched
  by CMake at configure time, so the first configure needs network access. Physics is no
  longer bit-exact across runs because Jolt uses a multithreaded job system.

### Fixed

- The orbit camera could be dragged below the ground plane (negative pitch, low target or
  a large zoom-out). The desired target height is clamped to the ground and the pitch is
  limited to the angle at which the orbit position would reach a 0.4 m clearance, both for
  the smoothed and the snapped state, so the view never dips under the terrain.
- `ParticleSystem::Reset` stored the absolute range start in the per-species cursor,
  but `AcquireSlot` treats the cursor as an offset inside the range. Every detonation
  therefore indexed past the species range and corrupted the heap, which showed up as
  intermittent crashes of the test suite and the application. The cursor is now reset to
  zero.
- Scene culling used `DirectX::BoundingFrustum` with the combined view-projection matrix,
  but that type expects a pure projection matrix. Objects and rubble disappeared at most
  camera angles. The six planes are now extracted directly from the view-projection
  matrix (`src/gfx/Frustum.h`) and covered by a regression test.
- Shadow casters just outside the camera frustum are kept so their shadows still reach
  into the visible area.

## [0.3.0] - 2026-10-25

### Added

- Procedural environment: a deterministic town of buildings, vehicles and trees around
  ground zero, generated from a seed with configurable density and per-type toggles.
  The analytic blast field drives pressure-impulse damage, window/facade charring,
  6-DOF rigid-body motion for vehicles and trees, building collapse into pre-fractured
  chunks, and burning embers/fire particles.
- Instanced scene renderer with shader-generated building windows, low-poly vehicles and
  trees, distance haze and a depth-writing opaque pass that correctly occludes the
  volumetric fireball and smoke.
- Scene settings (`sceneEnabled`, `sceneSeed`, `sceneDensity`, per-type toggles,
  `sceneQuality`), UI panel and `--scene-seed` / `--scene-density` / `--no-scene`.
- Scene tests: generation determinism, damage/collapse/fire behaviour and rigid-body
  settling.
- Absorbing sponge over the outer 6 % of the lateral and top solver boundaries. It
  relaxes the state towards ambient so outgoing acoustic waves do not reflect back into
  the plume; the ground remains a reflecting wall.
- Deterministic readback mode for headless captures: the solver waits for the reduction
  every 8 steps, so repeated captures produce identical images.
- Hybrid solver auto-scale: the analytic cloud drives the growth timing at 70 % of the
  domain, the measured product bounds are a hard safety net at 95 %, and the radial
  measurement ignores the base surge below 25 % of the domain height. This restores the
  undiffused mushroom cap while still preventing clipping.
- Low-burst energy conservation: when part of the initial charge sphere lies below the
  ground, the density and energy density are scaled by the full/above-ground volume ratio
  so the total charge mass and energy are preserved.
- Brode's near- and far-field branches are blended with a smoothstep over `Z = 0.85…1.0`,
  keeping the overpressure curve continuous and monotonic for the inverse solver.
- Z-dependent Friedlander decay coefficient; far-field waveforms keep their longer tail.
- Tests for the Brode crossover monotonicity and inverse radius, volume auto-scale
  containment, and volume-bake determinism.
- GPU solver smoke test (`gpu_tests`): creates a Direct3D 11 device (hardware or WARP),
  runs `Reset`/`Step`/`RescaleDomain` and checks the reduction bounds. It is skipped
  when no device is available.
- Settings round-trip test covering every persisted field.
- `URL_HASH` for the Dear ImGui download.

### Changed

- Warnings-as-errors (`/WX`) is enabled by default; the remaining unused-parameter
  warning in the shader include handler was removed.
- Solver CFL reduced from 0.30 to 0.20 and time steps are advanced in sub-batches of
  8 steps. Interactive runs poll the reduction through an event query without blocking;
  a rising maximum signal speed is picked up between sub-batches instead of after a
  whole batch.
- High-fidelity time captures now advance the analytic model together with the solver,
  so ground scorch, dust and the frame camera use the correct simulation time.
- The HF render pass no longer computes an unused density gradient (six texture loads per
  voxel), and the raymarcher computes its gradient-based sun shading once per pixel
  instead of once per step.
- Thermal constants and the volume extinction scale are centralised; the unreachable
  RGBA8 volume path and several unused helpers were removed.

### Fixed

- High-fidelity `--time` captures advanced the analytic blast model twice (once in
  `RunHeadless` and once through the solver advance).
- A stale bloom texture was still added to the composite after bloom was disabled.
- The high-fidelity frame camera now includes the wind drift applied to the rendered
  volume instead of framing the origin.
- The auto-scale radial trigger no longer reacts to the low base surge and coarsens the
  plume before the visible cloud needs it.

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

[0.3.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.3.0
[0.2.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.2.0
[0.1.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.1.0
