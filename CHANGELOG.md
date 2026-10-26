# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **GPU fire particles** - burning rubble and trees emit procedural flame billboards
  (`PSFire`): fbm turbulence shapes the silhouette and heat gradient in a single
  instanced draw call, with soft depth fading against the scene.
- A continuous **Chunk size** slider in *Environment* (0.25x to 2x, default 1x) that
  scales the average Voronoi cell volume. The per-building cell count is capped, so the
  smallest sizes saturate on large buildings.
- An **Anisotropic filtering** dropdown in *Rendering* (Off, 2x, 4x, 8x). It selects
  the anisotropic sampler for the volumetric raymarch and for the procedural surface
  detail, and supersamples the facade patterns along the major footprint axis, so
  windows and bricks stay sharp at grazing angles.
- The application starts in the armed/ready state and waits for **Detonate** (or **D**)
  instead of auto-detonating after the fuse.
- **D** triggers Detonate; **H** cycles the UI: collapse the side panel, hide the
  left-side overlays, then restore both.
- The control panel has an animated right-edge handle for collapsing and reopening it.
- High-resolution procedural materials: four building facade styles (glass, brick,
  concrete, industrial), vehicle paint, glass, lights and tires, and tree bark/foliage
  variation.
- The default procedural scene seed is **69707**.

### Changed

- Chunk texturing is static: each chunk keeps its building's colour and facade
  coordinates, and only the semi-transparent fire effects overlay it.
- The Pause/Rearm/Frame row at the top of the panel was removed. Pause and Frame now sit
  below the main Detonate/Rearm button in the same style.

### Fixed

- Burning trees and vehicles now show flames in both render modes. Fire is spawned on
  the visible canopy rim or roof surface instead of inside the mesh, where the soft depth
  fade hid it, and the spawn point follows the current rigid-body pose of thrown objects.
  The high-fidelity mode no longer drops the scene fire batch.

## [0.5.5] - 2026-10-26

This maintenance release keeps the high-fidelity solver responsive on small charges and
adds two user-facing controls: a per-frame solver budget with a Max FPS cap, and a
selectable hemispherical high-fidelity domain.

### Highlights

- **Balanced high-fidelity performance** - **Max steps/frame** bounds the solver work per
  frame. The playback time scale remains the target; when the budget is reached the
  simulation runs slower instead of collapsing the frame rate, and the UI warns.
- **Frame-rate cap** - a **Max FPS** dropdown with 30, 60, 90, 120 and 144 fps.
- **Hemispherical HF domain** - the **Model** menu switches between the cube and a masked
  semi-ellipsoidal dome. The Cartesian grid, CFL behaviour and cubic texture/memory layout
  are unchanged.

### Added

- A **Max FPS** dropdown in the Performance panel with 30, 60, 90, 120 and 144 fps caps.
  The wait is included in the wall-clock frame delta used by the high-fidelity target.
- The high-fidelity **Model** menu now offers a **hemisphere** domain in addition to the
  cube. It masks the existing Cartesian volume to a semi-ellipsoidal dome with a radial
  sponge and normalized auto-scale; the cubic texture/memory layout is unchanged.

### Changed

- High-fidelity solver work is capped by **Max steps/frame** (8..256). The playback time
  scale remains the target, but when the budget is reached the simulation runs slower
  instead of collapsing the frame rate; the UI warns when the budget is active.
  `solverStepsPerFrame` now means total steps per frame (default 96).

### Fixed

- Small-charge high-fidelity detonations could drop to a few fps at the start: the small
  domain produced a tiny CFL step, so covering the real-time target required thousands of
  solver steps per frame. The per-frame step budget keeps the frame time balanced across
  charge sizes.

## [0.5.0] - 2026-10-26

This release is the high-fidelity and procedural-scene update. It turns the GPU solver
into a mushroom-cap model with charge-scaled domains and direct time-scale playback,
extends the charge range to 100,000 t, makes the town a charge-scaled disc, adds the
responsive loading bar, the four-row FPS data box and the two-step Detonate/Rearm flow,
and fixes the large-charge Voronoi crash and the lateral auto-scale edge detection.

### Highlights

- **High-fidelity mushroom cap** - sub-grid drag and toroidal vortex closure, measured
  auto-scale, low-soot smoke rendering and a cloud-framing camera.
- **Charge range and solver cube** - a double-precision slider to 100,000 t with t/kt
  presets, and an HF domain that scales with the charge instead of a 30 m cap.
- **Large-charge stability** - fixed the Voronoi cap-sort crash, scaled the town with the
  destruction radius and arranged it as a disc.
- **Responsive interface** - worker-thread scene generation with a green loading strip, a
  rounded FPS data box, and the Detonate/Rearm flow.
- **Real-time HF playback** - the solver follows the playback time scale directly
  (1.0 = real time), with a 1 ms CFL cap so coarse grids can keep up; the auto-scale now
  catches the lateral smoke/fireball edge.

### Added

- A green loading strip along the bottom edge (5 % of the window height) shows the
  procedural scene generation progress. The interactive app builds the town on a worker
  thread and freezes the simulation while loading; headless captures stay synchronous.
- The on-screen data box now includes the frame rate and lists time, TNT equivalent,
  shock-front radius and FPS in four rows inside a rounded dark-grey panel that sizes
  itself to the text.
- The main Detonate button now switches to **Rearm** while an explosion is rendered.
  Rearm stops the solver, resets the scene and holds the fuse; the next click on
  **Detonate** starts the new explosion immediately.

### Changed

- High-fidelity mode now forms a mushroom cap instead of a narrow rising column. The
  auto-scale follows the measured plume (78 % trigger, 12 % margin) instead of expanding
  to the analytic cloud at 70 %, keeping the grid fine while the cap forms. A sub-grid
  drag and toroidal vortex-ring force around the analytic cloud radius spin the plume
  into a cap; the render uses a separate low soot threshold for the diluted smoke, and
  the camera frames the analytic cloud rather than the solver domain.
- Charge mass selection extended to 100,000 t. The slider uses double precision, shows
  whole kilograms from 100 kg upward, and adds 10 t / 100 t / 1000 t / 10,000 t buttons.
- The high-fidelity domain cube scales with the charge (`3 * R_fireball`, up to a 2 km
  half extent) instead of being capped at 30 m; the reduced-order volume box already
  follows the fireball and cloud.
- The procedural town is now a disc whose radius is the half diagonal of the previous
  square layout. Its base extent is `max(10 * R_fireball + 20, 1.4 * R_destroy + two
  blocks)` (45 m floor), so intact objects remain visible outside the destruction radius
  even for a 100 kt charge. The object budget scales with the circle area (capped at
  3200), blocks are sampled uniformly, and objects outside the disc or inside the
  destruction radius are rejected before their chunks are generated.
- High-fidelity playback is driven directly by the playback time scale: at 1.0 the
  solver advances one simulated second per real second. `solverStepsPerFrame` is now only
  a per-batch work cap, and the solver's CFL time step may grow up to 1 ms so coarse
  auto-scaled grids can actually keep up. The "Real time" readout is wall-clock time.

### Fixed

- The high-fidelity plume rose as a thin column because the auto-scale coarsened the grid
  before the vortex ring formed and the render's soot threshold hid the diluted cap.
- Large charges could crash while generating Voronoi building chunks (access violation in
  `Scene::Generate`). The cap polygon was sorted with `std::sort` and an inlined
  `std::atan2` comparator; under `/fp:fast` + AVX2 a near-degenerate cap could violate the
  strict-weak-ordering contract and make the insertion sort read past the cap array, which
  then indexed the vertex list with a garbage value. Caps are now ordered with precomputed
  angles and a bounded insertion sort, which cannot walk out of bounds. A regression test
  covers the exact 100 kt / seed 1357 cell.
- High-fidelity auto-scale sometimes did not grow the domain when the smoke or fireball
  reached the lateral boundary. A zero radius from the reduction could overwrite the last
  valid value, and the 25 % height floor ignored the visible smoke cap. The reduction now
  measures dilute smoke above the base surge and dense fireball products below it, ignores
  the ground layer faded out by the renderer, and never overwrites a valid radius with
  zero. The analytic cloud is a per-component fallback until that component is measured.

## [0.4.0] - 2026-10-25

### Added

- Procedural environment: a deterministic town of buildings, vehicles and trees around
  ground zero, generated from a seed with configurable density and per-type toggles. The
  analytic blast field drives pressure-impulse damage, window/facade charring, 6-DOF
  rigid-body motion for vehicles and trees, progressive building collapse into
  pre-fractured chunks, and burning embers/fire particles.
- Instanced scene renderer with shader-generated building windows, low-poly vehicles and
  trees, distance haze and a depth-writing opaque pass that correctly occludes the
  volumetric fireball and smoke.
- Scene settings (`sceneEnabled`, `sceneSeed`, `sceneDensity`, per-type toggles,
  `sceneQuality`, `sceneVoronoiChunks`), UI panel and `--scene-seed` / `--scene-density` /
  `--no-scene`.
- Progressive building collapse: per-edge joints with staggered failure times and a
  support flood fill (six grid neighbours or shared Voronoi faces), so chunks detach in
  waves; strongly loaded joints fail immediately, the rest within a few tenths of a second.
- Static structure colliders: chunks that are still standing become static boxes, so
  falling debris piles up against them. Detached chunks reuse the same body and are
  switched to dynamic, and contact impulses damage the struck chunk's joints, which can
  start a local cascade.
- Voronoi rubble (default): each building is split into convex cells clipped to its box.
  The cells keep flat-shaded hulls for rendering and a principal-component fitted OBB as
  the collision proxy; shared faces become the joints of the support graph. Hulls are
  drawn with one non-instanced draw call per building through a per-vertex chunk id.
- Jittered grid fallback: 2x3x2 at `scene.quality = 0`, 3x4x3 at `quality = 1` (the
  default) and 3x5x3 at `quality = 2`; split planes jitter by +-28 % per axis with a
  0.28-cell minimum, and the chunk mesh is a chamfered box (bevel 0.10 of the half
  extent).
- Building chunks keep the facade of the part they broke off from: every chunk carries a
  texture origin and scale that maps its mesh-local position back to the original building
  space; blast damage is a procedural char and crack overlay on the intact facade.
- Blast shielding with a 3x3x2 sample grid per receiver: each ray accumulates the optical
  depth of every building it crosses (`exp(-depth / 3 m)`), a diffraction floor keeps
  shadowed receivers from going to zero, and an optional `sceneGroundReflection` setting
  adds a height-dependent surface reflection.
- Cascaded directional shadows: three cascades (2048/1024/1024) with practical splits,
  5x5 PCF and 10 % blending into the next cascade (the next light frustum is extended
  backwards to cover the blend band). Intact buildings render once into a
  camera-independent world-space 4096^2 static map that is only invalidated by scene, sun
  or collapse changes; rubble, vehicles and trees are drawn into all dynamic cascades with
  a caster margin of `max(200 m, 2 * scene extent)`, so chunk shadows match the cached
  building shadows at every distance.
- Camera-frustum culling for intact objects and detached chunks, a distance-based fade of
  the procedural facade, sub-pixel culling of distant rubble, and a single-cone tree mesh
  beyond 80 m.
- Density scaling beyond the object budget: `scene.density` also packs `ceil(density)`
  smaller objects into each city block.
- Scene generation and benchmark logs report building/vehicle/tree, chunk and rigid-body
  counts, plus the number of awake bodies.
- `--distance-scale` now also applies to the reduced-order camera framing.
- High-fidelity mode follows the playback time-scale slider: the configured
  `solverStepsPerFrame` is the budget at the default 0.06 and is scaled by
  `timeScale / 0.06` (clamped to 1..96 steps).
- Scene tests: generation determinism, damage/collapse/fire behaviour, rigid-body
  settling, and Voronoi hull containment/adjacency/determinism.

### Changed

- **Rigid-body backend replaced by Jolt Physics 5.6.0 (MIT).** The in-house solver
  (uniform grid, SAT, sequential impulses, shock propagation) is gone; Jolt provides the
  BVH broadphase, persistent manifolds with warm starting, box inertia, island sleeping
  and a multithreaded solver. The `PhysicsWorld` interface is unchanged, body state is
  mirrored for the renderer and tests, contact impulses are estimated with
  `EstimateCollisionResponse`, and the left-handed scene is mirrored on Z at the boundary.
  At density 3 the initial integration reduced the 1920x1080 frame time from ~8.0/8.7 ms
  to ~5.8/4.2 ms (Voronoi/grid). Jolt is fetched by CMake at configure time, so the first
  configure needs network access; physics is no longer bit-exact across runs because Jolt
  uses a multithreaded job system.
- Voronoi chunks are now the default rubble representation; the jittered grid remains
  available through *Environment > Voronoi chunks*.
- The orbit camera cannot be moved below the ground plane: the target height is clamped
  to the ground and the pitch is limited to the angle that keeps a 0.4 m clearance.
- The README hero image was refreshed with a new mushroom-cloud capture.

### Fixed

- Rigid bodies were created in Jolt with the raw Z coordinate while the return path
  mirrored Z, so every body (including detached chunks) was mirrored across the Z axis.
  Buildings whose chunks detached therefore appeared to vanish from their location. The
  creation now uses the same mirror conversion; a round-trip test covers it.
- `ParticleSystem::Reset` stored the absolute range start in the per-species cursor, but
  `AcquireSlot` treats the cursor as an offset inside the range. Every detonation indexed
  past the species range and corrupted the heap, which showed up as intermittent crashes
  of the test suite and the application. The cursor is now reset to zero.
- Scene culling used `DirectX::BoundingFrustum` with the combined view-projection matrix,
  but that type expects a pure projection matrix. Objects and rubble disappeared at most
  camera angles. The six planes are now extracted directly from the view-projection
  matrix (`src/gfx/Frustum.h`) and covered by a regression test.
- Shadow casters just outside the camera frustum are kept so their shadows still reach
  into the visible area.

## [0.3.0] - 2026-10-25

### Added

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

[Unreleased]: https://github.com/Grownz/ExplosionSim/compare/v0.5.5...HEAD
[0.5.5]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.5.5
[0.5.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.5.0
[0.4.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.4.0
[0.3.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.3.0
[0.2.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.2.0
[0.1.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.1.0
