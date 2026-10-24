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

- 96³ by default (32…160 configurable), `R32G32B32A32_FLOAT`
- updated with `UpdateSubresource` when the baker publishes a new version
- linear clamp sampler, no mipmaps
- the shader jitters ray start positions to hide slice banding and adds procedural noise
  for sub-voxel detail

### Shaders

Shaders are loaded from `assets/shaders` at runtime, which makes it possible to iterate on
looks without rebuilding. Compilation errors are logged with file and line information.
The asset path is resolved relative to the executable first and then the working
directory.

## Determinism and headless mode

- Fixed particle seed per detonation
- Fixed-step integration in headless mode (1/120 s)
- Synchronous volume bake before the capture
- No temporal post-processing history

`--screenshot` therefore produces byte-identical images for the same command line and
GPU/driver combination.

## Performance notes

- Volume resolution dominates CPU cost: 96³ costs roughly 5–15 ms per bake on a modern
  8-core CPU, 128³ roughly twice that.
- Ray-march steps dominate GPU cost: 96 steps at 1600×900 is comfortable on a mid-range
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
