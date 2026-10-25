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

The UI reports the worker count and the measured bake time.

## Frame loop

```
process Win32 messages
├─ simulate (unless paused)
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

## Rendering pipeline

| Pass | Target | Notes |
|---|---|---|
| Sky | HDR RGBA16F | fullscreen ray, procedural gradient and clouds |
| Ground | HDR + depth | fullscreen ray/plane intersection, writes `SV_Depth` |
| Depth copy | R32F texture | avoids read/write hazards when sampling depth |
| Debris/sparks | HDR | instanced billboards, additive |
| Volume | HDR | ray-marched emission/absorption, premultiplied blend |
| Fire tongues | HDR | instanced billboards, additive |
| Smoke/dust | HDR | instanced billboards, alpha blend, soft depth fade |
| Bloom | 5 mip chain | 4-tap downsample, tent upsample, additive |
| Composite | swap chain / offscreen | ACES, exposure, vignette, dithering |

Opaque geometry writes depth; particles and the volume test against it but do not write.
The volume is drawn before the smoke particles so soot composites over the fireball.

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
- linear clamp sampler, no mipmaps
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

Two RGBA32F 3D textures hold the conserved variables:

| Texture | Contents |
|---|---|
| `StateA` | `(rho, rho*u, rho*v, rho*w)` |
| `StateB` | `(E, rho*Y, 0, 1)` |

Each variable has two textures for ping-pong updates. A render texture (RGBA16F) stores
`(density, temperature/12kK, dust, 1)` and is sampled by the existing volume raymarcher.

### Passes

1. **Init** – fills the domain with ambient air and deposits the charge as a sphere of
   detonation products.
2. **Sweep x / y / z** – finite-volume update with MUSCL reconstruction and HLLC fluxes.
3. **Source** – buoyancy relative to the ambient density and soot cooling.
4. **Reduce** – group-shared maximum of the signal speed `|v| + c`, written to a
   one-element structured buffer.
5. **Render** – converts the state into the render volume (soot incandescence, shock
   gradient, compression).

Three sweeps plus one source pass per step means the ping-pong index returns to its
starting value after each step. All passes are dispatched with `8x8x8` thread groups.

### Adaptive time step

The reduction result is copied into a staging buffer. The interactive path checks an
event query without blocking and keeps the previous maximum if the copy is still in
flight; headless captures wait for the copy so the step sequence is reproducible. The
step size is

```
dt = CFL * dx / max(|v| + c),   CFL = 0.20
```

clamped to `[0.1 us, 50 us]`. A batch is split into sub-batches of 8 steps so a rising
maximum signal speed is picked up without a long CFL overshoot. At start-up a
conservative 4000 m/s is assumed until the first reduction result is available.

### Domain and boundaries

The domain is a cube of side `2 * 3 * R_fireball` with the ground at `y = 0`. The ground
is a reflecting wall implemented by mirroring the ghost cell and reversing the vertical
momentum. The lateral and top boundaries use zero-gradient ghost cells plus an absorbing
sponge over the outer 6 % of the domain, which relaxes the state towards ambient so
outgoing acoustic waves do not reflect back into the plume.

### Auto-scale (domain expansion)

When auto-scale is enabled, the reduction pass also reports the bounding box of the
visible detonation products (maximum height and maximum `|x|/|z|` radius). The radial
measurement ignores the low base surge below 25 % of the domain height, which otherwise
expands the box long before the cloud needs it. The analytic cloud drives the growth
timing at 70 % of the domain; the measured bounds are a hard safety net at 95 %. When
either criterion fires, the renderer calls `RescaleDomain`, which grows the box by 1.3×
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
still fit; per-type toggles enable buildings, vehicles and vegetation. Each object stores
the free-air arrival time (`BlastModel::EstimateArrivalTimeS`), peak overpressure,
Friedlander impulse (`OverpressureImpulsePaS`) and thermal fluence evaluated at
generation, plus the number of buildings that block the line of sight to ground zero.

At the arrival time a pressure-impulse criterion sets the damage state
(intact/light/heavy/destroyed). Heavy or destroyed buildings are replaced by
pre-fractured convex chunks (2x3x2 by default; `scene.quality` changes the grid),
vehicles and trees become 6-DOF rigid bodies, and burning objects emit fire particles.
The rigid-body world uses an averaged ground contact point for stability, OBB-OBB
saturating-axis tests with a four-point contact manifold for body-body contacts, Coulomb
friction, contact damping and sleeping; it runs in fixed 1/120 s substeps. Damage events
are queued and drained by `Simulation`, which spawns debris and fire particles on the
main thread.

`SceneRenderer` draws the intact objects and active chunks as instanced procedural
meshes (box buildings with shader-generated windows, low-poly vehicles and trees). It is
split into a depth-only shadow pass (2048x2048 directional map, 3x3 PCF, camera-frustum
culling of casters) and a main pass that runs after the ground, so buildings correctly
occlude the fireball and smoke and cast shadows on the terrain. Distance-based level of
detail fades the procedural facade beyond 150 m and drops rubble chunks below one pixel.
A distance haze matches the ground and sky. Determinism is preserved by a seeded RNG,
fixed physics substeps and a generation hash exposed for tests.

### Integration with the application

- The `Renderer` owns the solver and detects a new detonation through
  `Simulation::BlastSerial()`.
- While the charge is armed no solver work is done; after detonation the solver is
  initialised and advanced by `solverStepsPerFrame` steps per rendered frame.
- The reduced-order model still advances, but its time is driven by the solver's
  simulated advance (`Simulation::AdvanceExternal`), so the camera, fireball light and
  readouts stay in sync.
- Particle rendering is skipped in this mode: the volume is the physical solution.
- `Renderer::PrepareHighFidelity` advances the solver to an exact target time for
  deterministic headless captures.
- The UI shows solver time, time step, maximum signal speed and total steps.

### Performance

At 128³ with 24 steps per frame the solver and renderer reach about 50 fps at 1600×900
on an RTX 3090. 96³ is roughly twice as fast. The GPU work is dominated by the three
sweeps; the reduction and render passes are comparatively cheap. 256³ is about eight
times more expensive per step, 512³ about 64 times; those grids are intended for stills
or very slow motion. A VRAM guard rejects a grid that would need more than 75 % of the
dedicated video memory.

## Determinism and headless mode

- Fixed particle seed per detonation
- Fixed-step integration in headless mode (1/120 s)
- Synchronous volume bake before the capture
- No temporal post-processing history

`--screenshot` therefore produces byte-identical images for the same command line and
GPU/driver combination.

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

## Extending

The volume field is produced in `VolumeBaker::BakeRange`, which fills the RGBA texture
directly. Replacing the analytic field with a numerical solver (for example an
axisymmetric compressible Euler solver) only requires writing density, temperature and
dust into the same buffer; the renderer and the rest of the application stay unchanged.
