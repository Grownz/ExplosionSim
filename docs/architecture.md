# Architecture

## Layers

```
┌─────────────────────────────────────────────────────────────┐
│ app::App            Win32 window, frame loop, input, CLI    │
├──────────────────────────┬──────────────────────────────────┤
│ ui::Ui (Dear ImGui)      │ gfx::Renderer (Direct3D 11)      │
├──────────────────────────┴──────────────────────────────────┤
│ sim::Simulation                                              │
│   ├─ BlastModel        reduced-order physics                 │
│   ├─ ParticleSystem    debris / sparks / fire / smoke / dust │
│   └─ VolumeBaker       analytic field → 3D texture data      │
├─────────────────────────────────────────────────────────────┤
│ core::ThreadPool, core::Rng, Log                             │
└─────────────────────────────────────────────────────────────┘
```

`explosion_core` (everything except `app`, `gfx` and `ui`) has no Direct3D dependency and
is covered by the unit tests. Dear ImGui is fetched by CMake and compiled into the
executable.

## Threading model

A single work-stealing pool is created at start-up with `hardware_concurrency() - 1`
workers. The calling thread always executes queued work while it waits, so a
`ParallelFor` cannot deadlock even if every worker is busy.

Two kinds of work are submitted:

1. **Particle integration** – `ParticleSystem::Update` splits the particle array into
   chunks of 2048 and integrates them in parallel. Each particle is independent; random
   numbers used during spawning are drawn on the main thread, so results are
   deterministic.
2. **Volume baking** – `VolumeBaker` dispatches one chunk per z-slice through
   `ParallelForAsync`. When the last slice finishes, an atomic flag is set. The main
   thread adopts the finished frame on the next update and uploads it to the GPU.

The bake result is double-buffered. A new bake writes into the buffer that is not
currently displayed, so the renderer never samples a partially written volume.

In the interactive app the procedural town is generated on a dedicated `std::jthread`
into a separate `Scene`. Placement is serial and deterministic; the Voronoi fracture runs
on the shared thread pool with one RNG per building. The worker first publishes a
layout-only preview (no chunks), so the intact town appears in ~10 ms; the previous town
stays visible until that preview is adopted. The full scene replaces the preview when the
chunks and shielding are ready. The camera frames the preview, and gameplay actions are
ignored while loading. A progress callback publishes 0..1 through an atomic, and the
main thread swaps the finished scene into place. The simulation is frozen while the town
is generated, so the countdown and the solver cannot advance into a half-built scene.
Headless captures and tests keep the synchronous path.

Rearm, Reset, loop restarts and non-scene setting changes reuse the deterministic town:
only the mutable object/chunk state (damage, burning, bodies, joints) is reset. A new
town is generated only when the layout settings or the charge/environment change.

The UI reports the worker count and the measured bake time.

## Frame loop

```
process Win32 messages
├─ poll scene generation (adopt finished town, re-frame on first load)
├─ simulate (unless paused or loading the town)
│   ├─ fuse countdown / detonation
│   ├─ BlastModel::Advance          (shock integration, fireball, cloud)
│   ├─ ParticleSystem::Update       (parallel, spawning on the main thread)
│   ├─ VolumeBaker::Poll            (adopt finished bake)
│   └─ VolumeBaker::StartBake       (max. 30 Hz, async)
├─ ImGui new frame + control panel
├─ handle camera / keyboard
└─ Renderer::Render + ImGui draw + Present
```

`StepFixed` provides deterministic stepping for headless captures: it does not use the
real-time clock and can be driven to an exact simulation time.

The main Detonate button becomes **Rearm** once an explosion is being rendered. Rearm
stops the solver, resets the simulation state and volume, and holds the fuse; the next
**Detonate** press starts the new explosion immediately. The generated town is reused
unless the scene layout or the charge changed.

## Rendering pipeline

| Pass | Target | Notes |
|---|---|---|
| Sky | HDR RGBA16F | fullscreen ray, procedural gradient and clouds |
| Ground | HDR + depth | fullscreen ray/plane intersection, writes `SV_Depth` |
| Depth copy | R32F texture | avoids read/write hazards when sampling depth |
| Debris/sparks | HDR | instanced billboards, additive |
| Fire tongues | HDR | instanced billboards, additive, drawn before the volume |
| Volume | HDR | ray-marched emission/absorption, premultiplied blend, occludes/scatters over the fire |
| Smoke/dust | HDR | instanced billboards, alpha blend, soft depth fade |
| Bloom | 5 mip chain | 4-tap downsample, tent upsample, additive |
| Composite | swap chain / offscreen | ACES, exposure, vignette, dithering |

Opaque geometry writes depth; particles and the volume test against it but do not write.
The flame pass runs before the volume: the smoke's premultiplied transmittance then
occludes the flames and the volume's scattering composites over them. The volume is
drawn before the smoke particles so soot composites over the fireball. Fire billboards
use `PSFire`: two fbm lookups shape a rising flame silhouette and a red-orange-white
heat ramp, and the result is additively blended with a soft depth fade. The CPU only
advances the emitter positions, so the whole fire pass is one instanced draw call.
Burning objects spawn their emitters on the visible canopy rim or roof surface and follow
the current rigid-body pose, so flames are not hidden inside the mesh.

### Volume texture

- 128³ by default (64…1024 configurable), `RGBA16F` up to 512³ and two `RG8` planes
  for 1024³ (the 4 GiB D3D11 per-resource limit makes a single 1024³ RGBA texture
  impossible)
- one CPU buffer with an upload handshake; high resolutions are baked at a reduced rate
- auto-scale bounds (default) follow the fireball, smoke envelope, cap and base dust
  with a 10 % margin; the box only grows and the fixed voxel count means the effective
  accuracy drops as the box grows (the UI shows the box size and voxel size)
- a tileable 64³ RGBA8 noise volume provides domain warping and fine detail in the
  raymarcher
- the raymarcher binds the noise volume at `t2` and uses it both to warp the sample
  position (filaments) and to modulate density at sub-voxel scale
- updated with `UpdateSubresource` when the baker publishes a new version
- linear clamp sampler by default; *Rendering > Anisotropic filtering* selects a
  2x/4x/8x anisotropic clamp sampler for the raymarch and a wrap variant for the
  procedural surface grain
- no mipmaps
- the ground and scene shaders march the same volume towards the sun (8 samples) for
  the smoke shadow; the raymarcher uses a 6-sample sun transmittance for self-shadowing
- the first 3 m of the volume ray are faded out so flying into the cloud does not leave
  a screen-filling veil
- the shader jitters ray start positions to hide slice banding and adds procedural noise
  for sub-voxel detail

### Shaders

Shaders are loaded from `assets/shaders` at runtime, which makes it possible to iterate on
looks without rebuilding. Compilation errors are logged with file and line information.
The asset path is resolved relative to the executable first and then the working
directory.

## High-fidelity GPU solver

`gfx::GpuEulerSolver` is an optional 3D compressible Euler solver that runs in Direct3D
11 compute shaders and replaces the CPU-baked volume with a numerical solution.

### State layout

Two 3D textures hold the conserved variables:

| Texture | Format | Contents |
|---|---|---|
| `StateA` | RGBA32F | `(rho, rho*u, rho*v, rho*w)` |
| `StateB` | RG32F | `(E, rho*Y)` |

`StateB` only carries the total energy and the detonation-product mass, so packing it into
two channels removes 25 % of the state memory and state bandwidth without touching the
physical precision. Each variable has two textures for ping-pong updates. A render texture
(RGBA16F) stores `(density, temperature/12kK, dust, 1)` and is sampled by the existing
volume raymarcher.

### Passes

1. **Init** – fills the domain with ambient air and deposits the charge as a sphere of
   detonation products.
2. **Sweep x / y / z** – finite-volume update with MUSCL reconstruction and HLLC fluxes.
   The Z sweep also applies the source terms (buoyancy, entrainment drag, cap closure,
   soot cooling, absorbing sponge) to the state it just produced, so the state does not
   round-trip through memory. The order is still X, Y, Z, Source and the equations are
   unchanged. Each direction has its own entry point (`CSSweepX` / `CSSweepY` /
   `CSSweepZ`): the direction constant folds the HLLC selects, and the fused source code
   stays out of the X/Y shaders (it raises their temp-register count from 22 to 24 and
   would cost about 12 % of the total solver time). The middle MUSCL stencil is shared by
   both faces of a cell, so 18 minmods are evaluated instead of 24.
3. **Reduce** – group-shared maximum of the signal speed `|v| + c`, written to a
   one-element structured buffer.
4. **Render** – converts the state into the render volume (soot incandescence, shock
   gradient, compression). It runs once per frame, after all solver steps, not after
   every sub-batch.

Three sweeps per step mean the ping-pong index returns to its starting value after each
step. All passes use `4x4x4` thread groups (the group size is compiled in through
`EULER_GROUP_X/Y/Z` and was tuned on an RTX 3090: 4³ 121 ms, 8³ 137 ms, 16×8×8 146 ms and
10³ 153 ms at 128³/256 steps). The sweep loads the centre cell once and reuses its
conserved and primitive forms for the update.

The solver keeps a ring of four constant buffers. The sweep/source constants change only
when `dt` changes, so they are written once per 8-step sub-batch; the render pass refreshes
them once per frame for its time-based incandescence. Only the two state UAVs are unbound
between passes. `--solver-benchmark` reports the CPU submission time separately: on the
reference machine it is ~1.1 ms for 256 steps, far below the GPU time, so the frame time is
GPU-bound.

### Solver-only benchmark

`--solver-benchmark` runs the solver without the scene or the raymarcher and reports
per-pass GPU times from `D3D11_QUERY_TIMESTAMP` / `D3D11_QUERY_TIMESTAMP_DISJOINT`:

```
ExplosionSim.exe --solver-benchmark --solver-resolution 128 --solver-step-count 256
```

The benchmark uses the non-blocking reduction path and consumes the reduction at frame
boundaries, so the numbers match the interactive pipeline instead of a readback-bound
headless capture. `--solver-steps` sets the steps per frame (default: the configured
`solverStepsPerFrame`). The domain is fixed, because auto-scale would change the grid
between resolutions.

### Adaptive time step

The reduction result is copied into a staging buffer. The interactive path checks an
event query without blocking and keeps the previous maximum if the copy is still in
flight; headless captures wait for the copy so the step sequence is reproducible. The
step size is

```
dt = CFL * dx / max(|v| + c),   CFL = 0.20
```

clamped to `[0.1 us, 1 ms]`. A batch is split into sub-batches of 8 steps so a rising
maximum signal speed is picked up without a long CFL overshoot. At start-up a
conservative 4000 m/s is assumed until the first reduction result is available.

### Domain and boundaries

The domain is a cube of half extent `3 * R_fireball` (1 m to 2 km) with the ground at
`y = 0`. The ground
is a reflecting wall implemented by mirroring the ghost cell and reversing the vertical
momentum. The lateral and top boundaries use zero-gradient ghost cells plus an absorbing
sponge over the outer 6 % of the domain, which relaxes the state towards ambient so
outgoing acoustic waves do not reflect back into the plume.

### Domain shape

The high-fidelity solver keeps its uniform Cartesian grid. `solverHemisphere` masks the
active cells to a semi-ellipsoid with lateral radius `0.5 * (max.x - min.x)` and vertical
semi-axis `max.y`; the ground remains the reflecting plane. Cells outside the mask are
filled with ambient air, the sweep/source/render passes early-out, and the curved boundary
uses the radial sponge. The reduction reports a normalized dome fill and the auto-scale
expands when it approaches 78 %. The 3D texture stays cubic, so memory use is unchanged;
the boundary is stair-stepped at voxel resolution. The Model menu selects cube or
hemisphere.

### Sub-grid cap closure

The coarse grid cannot resolve the vortex ring that forms the mushroom head. The source
pass adds a drag term `0.6 v|v| / (2R)` (the same entrainment closure as the analytic
cloud) and a toroidal force around the analytic cloud radius and centre: outward at the
cap top, downward outside, inward at the bottom and upward inside. The render conversion
uses a lower soot threshold for the diluted smoke than for the incandescent products, and
the camera frames the analytic cloud rather than the solver domain.

### Auto-scale (domain expansion)

When auto-scale is enabled, the reduction pass also reports the bounding box of the
visible detonation products (maximum height and maximum `|x|/|z|` radius). The radial
measurement has two tiers: above 25 % of the domain height every visible smoke cell
counts, while below it only dense fireball products count, so the fireball's equator is
measured without reacting to the low, dilute base surge. The lowest layer faded out by
the renderer is ignored. A zero radius is never written over a previously measured one,
and the analytic cloud is a per-component fallback until the reduction has measured that
component. The measured bounds drive the expansion: the domain grows when the plume
approaches 78 % of the boundary, with a 12 % margin, so the grid stays as fine as
possible while the cap forms. When the criterion fires, the
renderer calls `RescaleDomain`, which grows the box by 1.3×
(up to an accuracy factor of 16). The expansion is repeated if one step is not enough, so
a fast plume rise is caught before it clips. A compute pass resamples the six conserved
variables trilinearly onto the new grid (ambient outside the old domain) using the
existing ping-pong state textures. Because the voxel count is fixed, `dx` grows, the CFL
step grows and the number of steps for a physical time interval drops. The renderer
reports the domain, voxel size, measured plume top/radius, accuracy factor and expansion
count.

### Procedural scene

`Scene` generates a deterministic town around ground zero from `scene.seed`: a jittered
block grid with buildings, vehicles on the road centre lines and trees along the
sidewalks. `scene.density` scales both the object budget (800 at 1.0) and the number of
objects per block (`ceil(density)`), with their footprints shrunk so that extra copies
still fit; per-type toggles enable buildings, vehicles and vegetation. The town is a disc
whose radius is the half diagonal of the previous square layout
(`sqrt(2) * max(10 * R_fireball + 20, 1.4 * R_destroy + two blocks)`, 45 m floor), so
intact objects always exist outside the destruction radius even for a 100 kt charge. The
budget grows with the circle area (π/2 relative to the old square, capped at 3200) and
blocks are sampled uniformly when the grid is larger than the budget; objects outside the
disc or inside the destruction radius are rejected before their chunks are generated.
The default seed is 69707. Buildings select one of four procedural facade styles
(glass, brick, concrete, industrial); vehicles and trees add high-frequency paint, glass,
lamp, bark and foliage detail in the scene pixel shader.

Each object stores
the free-air arrival time (`BlastModel::EstimateArrivalTimeS`), peak overpressure,
Friedlander impulse (`OverpressureImpulsePaS`) and thermal fluence evaluated at
generation, plus the number of buildings that block the line of sight to ground zero.

At the arrival time a pressure-impulse criterion sets the damage state
(intact/light/heavy/destroyed). Heavy or destroyed buildings are split into pre-fractured
chunks along jittered planes (2x3x2 by default, 3x4x3 at `scene.quality = 1` and 3x5x3 at
`quality = 2`), so no two neighbours share the same dimensions. The split planes move by
up to +-28 % of a cell but never closer than 0.28 cell to their neighbour, which keeps
every chunk usable as an OBB while removing most of the grid's right angles. With
`scene.voronoiChunks` (the default) the building is instead split into convex Voronoi
cells clipped to its box; each cell keeps a flat-shaded hull for rendering, a PCA-fitted
OBB as the collision proxy, and its shared faces define the support joints. The cap
polygon of each clipped cell is ordered with precomputed angles and a bounded insertion
sort: an inlined `std::atan2` comparator fed to `std::sort` could violate the
strict-weak-ordering contract under `/fp:fast` + AVX2 and walk past the cap array for
near-degenerate cells, which crashed large scenes. Vehicles and trees
become 6-DOF rigid bodies, and burning objects emit fire particles.

`scene.chunkScale` (0.25–2.0, default 1.0) multiplies the target Voronoi cell volume by
`scale³`; the cell count is clamped to 6..256 per building, so the smallest settings
saturate on large buildings. A global budget of **32,000 chunks** caps the whole scene:
the per-building counts are scaled down before the fracture. The fracture uses a flat
face-index polyhedron, hash-welded vertices, AABB early-outs for distant bisectors and
direct face-adjacency construction. Measured on the reference 8-core/16-thread CPU:
16k -> 0.35 s, 24k -> 0.7 s, 32k -> 0.77 s and 40k -> 1.2 s generation time; the default
20 t town (9,595 chunks) generates in ~0.15 s. Chunk
colours are static: every chunk inherits its object's
colour and keeps the building-space facade coordinates, and only the semi-transparent
fire effects overlay them.

Collapse is progressive. Each chunk edge carries a joint; the blast load sets a failure
time (strongly loaded joints fail almost immediately, the rest over the next few tenths of
a second). A priority list of pending failures is processed every update: the joint is
broken, the support graph is flood-filled again from the ground, and only the chunks that
lost their connection detach. Chunks that are still standing are registered as static
collision boxes, so debris piles up against the remaining structure; a heavy impact
reports a contact impulse back to the scene, which damages the struck chunk's joints and
can start a local cascade.

The rigid-body world is backed by Jolt Physics (MIT): box shapes, a BVH broadphase,
persistent manifolds with warm starting, analytic box inertia, island sleeping and a
multithreaded solver. Bodies live in two object layers (standing structure and moving
debris) and a large static box provides the ground plane. The scene keeps DirectX's
left-handed world and mirrors Z at the Jolt boundary; `PhysicsWorld` copies the body
state back after each step so the renderer and tests see the usual `RigidBody` array. A
contact listener turns static-dynamic impacts into `ContactEvent`s and estimates the
impulse with `EstimateCollisionResponse`. It runs in fixed 1/120 s substeps. Damage events
are queued and drained by `Simulation`, which spawns debris and fire particles on the main
thread.

Blast shielding traces 3x3 rays from the burst to the receiver, accumulates the optical
depth of every building the ray crosses (`exp(-depth/3 m)`), applies a diffraction floor
around the nearest occluder and, optionally, a ground-reflection factor near the surface.

`SceneRenderer` draws the intact objects and active chunks as instanced procedural
meshes (box buildings with shader-generated windows, low-poly vehicles and trees). Chunks
use a chamfered box mesh and carry a per-instance texture origin and scale; the shader
maps the mesh-local position back into the original building space, so each fragment
keeps the facade it broke off from. Blast damage is a procedural char and crack overlay
on that facade rather than a flat recolour. The facade pattern is supersampled along the
major screen-space footprint axis (2 taps for 2x, 4 taps for 8x) and a tiling grain
texture is sampled with the matching anisotropic sampler, so the filtering setting also
applies to the procedural objects. With `scene.voronoiChunks` (the default) the
rubble is a convex hull instead: all hulls live in one vertex/index buffer, each vertex
names its chunk, and a per-chunk structured buffer supplies the rigid transform, so a
whole building is one draw call. The renderer is
split into a depth-only shadow pass and a main pass that runs after the ground. Intact
buildings render once into a camera-independent world-space shadow map (4096^2) that is
only invalidated by scene, sun or collapse changes; the dynamic cascades
(2048/1024/1024) contain the rubble hulls, vehicles and trees in every cascade and use
5x5 PCF, so the debris casts dynamic shadows onto the ground, the intact buildings and
other rubble. Chunk casters are kept up to `max(200 m, 2 * scene extent)` outside the
camera frustum, matching the static map's coverage. The last 10 % of each cascade blends
into the next one, and the next cascade's light frustum is extended
backwards to cover the blend band. Trees switch to a single-cone mesh
beyond 80 m and the procedural facade fades beyond 150 m. A distance haze matches the
ground and sky. Determinism is preserved by a seeded RNG, fixed physics substeps and a
generation hash exposed for tests.

### Integration with the application

- The `Renderer` owns the solver and detects a new detonation through
  `Simulation::BlastSerial()`.
- While the charge is armed no solver work is done; after detonation the solver is
  advanced once per rendered frame to the playback target: `realDt * timeScale`
  simulated seconds, with 1.0 meaning real time. The solver's CFL step is smaller, so
  `AdvanceSolver` submits enough steps to cover the target, but the total is capped by
  `solverStepsPerFrame` (8..256). When the budget is reached the solver advances less
  than the target and playback runs slower; the UI warns. This keeps the frame time
  balanced across charge sizes: small charges have a small domain, a small CFL step and
  would otherwise demand thousands of steps per frame. The explicit time step follows the
  CFL condition with a 1 ms upper bound, so coarse auto-scaled grids can keep up with
  real-time playback. The "Real time" readout is wall-clock time. The Performance panel
  can cap the interactive frame rate at 30/60/90/120/144 fps; the wait is part of the
  wall-clock delta used by the HF target.
- The reduced-order model still advances, but its time is driven by the solver's
  simulated advance (`Simulation::AdvanceExternal`), so the camera, fireball light and
  readouts stay in sync.
- The analytic blast particle batches are skipped in this mode: the volume is the
  physical solution. Scene fires on burning objects are still simulated and rendered,
  because the analytic blast model drives the scene damage in both modes.
- `Renderer::PrepareHighFidelity` advances the solver to an exact target time for
  deterministic headless captures.
- The UI shows solver time, time step, maximum signal speed and total steps.

### Performance

At 128³ with 24 steps per frame the solver and renderer reach about 50 fps at 1600×900
on an RTX 3090. 96³ is roughly twice as fast. The GPU work is dominated by the three
sweeps; the reduction and render passes are comparatively cheap. 256³ is about eight
times more expensive per step, 512³ about 64 times; those grids are intended for stills
or very slow motion. Real-time playback (time scale 1.0) submits many more steps and
lowers the frame rate; coarse auto-scaled grids use the larger CFL step (up to 1 ms).
A VRAM guard rejects a grid that would need more than 75 % of the
dedicated video memory.

## Determinism and headless mode

- Fixed particle seed per detonation
- Fixed-step integration in headless mode (1/120 s)
- Synchronous volume bake before the capture
- No temporal post-processing history

`--screenshot` therefore reproduces the same scene and rendering for the same command
line and GPU/driver combination. The rigid-body solver uses Jolt's multithreaded job
system, so the exact rubble state can vary slightly between runs; captures that depend on
the debris layout are not guaranteed to be bit-identical.

## Performance notes

- Volume resolution dominates CPU cost: 128³ costs roughly 10–25 ms per bake on a modern
  8-core CPU, 96³ roughly half that.
- Ray-march steps dominate GPU cost: 128 steps at 1600×900 is comfortable on a mid-range
  GPU; 192 steps is intended for stills.
- Particle count can be reduced in the UI; 40 000 particles is the default and leaves
  plenty of headroom.
- The particle integration and the volume baker share the pool, so the UI may show
  occasional bake spikes when both run in the same frame. The bake is deliberately
  throttled and skipped while another bake is in flight.

## Updates

`update::UpdateChecker` owns one worker thread. It queries the GitHub releases API
(`/repos/Grownz/ExplosionSim/releases/latest`) with WinHTTP, parses the response with
nlohmann/json, compares the semantic version against `EXPLOSION_VERSION_STRING` and exposes
a snapshot that the UI polls every frame. The automatic check runs at most once per 24
hours (`lastCheckUnix` in `settings.ini`); a manual check bypasses the cache.

Downloads are user-initiated. The Win64 zip is written to
`%LOCALAPPDATA%\ExplosionSim\updates` with progress and cancel support. Installation is
user-initiated as well: the standalone `ExplosionSimUpdater.exe` waits for the application
to exit, extracts the zip with miniz, replaces the executable and assets, preserves
`settings.ini` and restores the previous files if the copy fails. The download host and
asset name are validated.

The changelog parser (`app::ParseChangelog`) reads the bundled `CHANGELOG.md`, splits it
into release sections and provides the current release body. The first start after a
version change opens the "What's new" modal; the footer button opens the full history.
The Markdown subset renderer in `Ui.cpp` draws headings, bullets, inline code and tables
directly with ImGui.

## Extending

The volume field is produced in `VolumeBaker::BakeRange`, which fills the RGBA texture
directly. Replacing the analytic field with a numerical solver (for example an
axisymmetric compressible Euler solver) only requires writing density, temperature and
dust into the same buffer; the renderer and the rest of the application stay unchanged.
