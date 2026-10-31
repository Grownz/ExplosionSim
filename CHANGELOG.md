# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.12.2] - 2026-10-31

0.12.2 is a packaging and update-dialog fix over 0.12.1. The Windows release
now ships `CHANGELOG.md`, so fresh installs have the changelog and updates
replace it; the Windows update panel also shows the new release's notes before
installing.

### Highlights

- **Changelog in the Windows package** - the release zip and the update payload
  now include `CHANGELOG.md`, so the in-app changelog no longer stays at the
  old version after an update.
- **Release notes before installing** - the Windows update panel shows the
  available release's notes, matching the Linux panel.

### Fixed

- **Changelog in the Windows package** - the release zip omitted
  `CHANGELOG.md`, so fresh installs had no changelog and updates never replaced
  the installed copy. The file is now part of the artifact and therefore of the
  update.
- **Release notes in the Windows update dialog** - the update panel now shows
  the available release's notes in a collapsible section, matching Linux.

## [0.12.1] - 2026-10-31

0.12.1 is a splash and update patch over 0.12.0. The startup splash uses the
same TTF family as the application again, blends between its stills and no
longer shows dark seams; the main window keeps the splash's z-level when it
closes. The Linux build gained the in-app updater that the Windows build
already had.

### Highlights

- **Splash polish** - shared TTF text, crossfaded stills, a seam-free gradient
  and a main window that stays in front when the splash closes.
- **Linux in-app updater** - `ExplosionSimUpdater` checks GitHub, verifies the
  SHA-256 digest, swaps the per-user install and restarts the application.
- **Headless splash and updater tests** - both run in CI on the matching
  platforms.

### Fixed

- **Splash font** - the splash now renders with the same TTF family as the
  ImGui panels (Segoe UI on Windows; DejaVu Sans, Liberation Sans or Noto Sans
  on Linux) instead of SDL's 8x8 bitmap debug font, so the startup screen
  matches the rest of the program again.
- **Splash gradient seams** - the dark bottom gradient is drawn as one texture
  instead of 32 overlapping strips, which left a dark horizontal line at every
  strip boundary.
- **Splash crossfade** - stills now blend into each other instead of snapping.
  The fade duration follows the measured loading speed (60 % of the time
  between two images, clamped to 120-1000 ms), so a fast load fades quickly and
  a slow load fades gently.
- **Window stays in front after the splash** - the splash is always-on-top, so
  the main window used to drop behind other applications when it closed. The
  window is now shown and focused while the splash is still the foreground
  window (Windows keeps it topmost until it has the foreground, then drops the
  flag; Linux raises it through SDL).

### Added

- **Linux in-app updater** - `ExplosionSimUpdater` is a separate helper binary
  shipped next to the application. It checks the GitHub releases API, downloads
  the `linux-x86_64.tar.gz` with progress, verifies the GitHub SHA-256 digest,
  extracts it, swaps the install directory atomically (with rollback) and
  relaunches the application. The ImGui panel gained an Updates section with
  check, download, install and "skip this version"; the daily check and the
  skipped version are stored in `settings.ini`. The helper owns all HTTP/TLS
  code (vendored mbedTLS), so the application itself has no network dependency.
  Automatic updates only run for per-user installs; a system-wide install
  should be updated through the package manager.
- A headless `splash` test on Windows and Linux that checks the shared TTF text
  and samples a frame in the middle of a crossfade.
- A Linux `updater` test covering SHA-256, URL validation, tar.gz extraction and
  the install swap including the relaunch.
- The Linux CI installs `fonts-dejavu-core` so the shared font is always
  available.

### Known limitations

- The Linux updater manages per-user installations only; a system-wide install
  should be updated through the package manager.
- The Linux updater does not use an explicit proxy setting yet.

## [0.12.0] - 2026-10-31

0.12.0 is the first public release of the native Linux port, building on the
0.11.0 milestone. Windows and Linux are now released together: the Windows
build keeps its Direct3D 11 path and the Linux build ships as a self-contained
SDL3 + Vulkan tar.gz. The release also includes the final rendering and
platform-parity work from the port.

### Highlights

- **Native Linux release** - `ExplosionSim-linux-x86_64.tar.gz` with the
  reduced-order renderer, the high-fidelity GPU Euler solver, GPU volume bake
  and particles, the SDL3 splash and the ImGui control panel.
- **Windows and Linux artifacts** - the release workflow attaches
  `ExplosionSim-win64.zip` and `ExplosionSim-linux-x86_64.tar.gz`.
- **Real scene-depth occlusion** - the volume, particle and shock passes sample
  the real scene depth and are occluded by buildings.
- **SDL3 splash on both platforms** - Windows now runs the same SDL3 splash on
  a worker thread; the GDI/WIC splash is gone.
- **Linux ImGui UI** - an F1 panel for blast/solver/render settings,
  pause/detonate/reset and camera framing.

### Added

- Real scene-depth sampling for the volume, particle and shock passes; the
  ground pass keeps the far-depth proxy because it renders inside the depth
  pass.
- SDL3 splash on Windows (threaded) and Linux (cooperative), with the portable
  stb image loader.
- ImGui SDL3/Vulkan control panel on Linux, including live render settings and
  blast reconfiguration.
- `package_linux` tar.gz with `install.sh`, desktop entry and icon.
- Tag-triggered release workflow that publishes both platform artifacts.
- Vulkan validation-layer and ASan/UBSan CI jobs, plus the Linux `physics` and
  `gpu_vulkan` test suites.

### Changed

- WIC replaced by stb_image/stb_image_write on both platforms.
- Settings use XDG paths on Linux and `%LOCALAPPDATA%` on Windows, while
  preferring a portable `settings.ini` next to the executable.
- The Linux solver benchmark reports per-pass GPU timestamps.
- Volume descriptor updates are cached and the Vulkan device uses a pipeline
  cache.

### Known limitations

- Linux v1 has no in-app self-updater; replace the release archive to update.
- The ground pass samples a far-depth proxy because it renders inside the main
  depth pass; the volume, particle and shock passes sample the real scene depth
  and are occluded by buildings.
- The same-GPU-class performance comparison is pending; lavapipe benchmarks are
  functional baselines only.

## [0.11.0] - 2026-10-31

0.11.0 is the native Linux port milestone. The same simulation core, scene
generator and GPU Euler solver now run through a backend-neutral RHI on Vulkan
and D3D11, and the Windows application keeps its existing D3D11 path. The Linux
shell ships as a self-contained tar.gz with the reduced-order renderer, the
high-fidelity solver, GPU volume bake/particles, an SDL3 splash, portable
settings and CI-verified capture parity.

### Added

- **Native Linux application** - `explosion_linux` builds with SDL3 + Vulkan,
  renders the reduced-order chain (sky, ground, procedural town, volume,
  particles, shock, bloom, composite) and runs the high-fidelity GPU Euler
  solver with render and light volumes.
- **Linux ImGui UI** - a settings/controls panel (F1) for blast, solver and
  render settings, pause/reset/detonate and camera framing.
- **Multi-platform RHI** - backend-neutral device, command list, compute,
  storage-image/buffer, indirect-draw and timestamp-query interfaces with D3D11
  and Vulkan implementations.
- **GPU Euler solver on Vulkan** - `RhiEulerSolver` ports the state ping-pong,
  init/sweep/reduce/render/light/rescale passes and the fused source terms.
- **GPU volume bake and particles** - the analytic volume bake and the
  spawn/reset/integrate/finalize particle path run as compute shaders, with CPU
  fallbacks (`--no-gpu-volume-bake`, `--no-gpu-particles`).
- **GPU timestamp benchmark** - the Linux solver benchmark reports the GPU total
  and per-pass sweep/reduce/render timings, matching the Windows benchmark.
- **Portable image I/O** - stb_image/stb_image_write replace WIC for PNG/JPEG
  loading and PNG saving.
- **SDL3 Linux splash** - the 980x640, 20-frame, 5 %-step splash with
  `--splash-test` and `--splash-screenshot`.
- **Portable settings** - XDG paths on Linux, `%LOCALAPPDATA%` on Windows,
  portable `settings.ini` preferred, and `--write-settings` with an exact
  round-trip.
- **Linux package** - `package_linux` produces a self-contained tar.gz with the
  binary, assets, desktop entry, icon and README; CI extracts and runs it.
- **Test matrix** - Linux `core`, `physics` and `gpu_vulkan`; Windows `core`,
  `physics` and `gpu`.
- **Benchmarks and hardening** - frame benchmark, descriptor-update caching,
  Vulkan pipeline cache, minimized-window rendering pause, high-DPI pixel sizes
  and a non-ASCII settings-path test.

### Known limitations

- Linux v1 has no in-app self-updater; replace the release archive to update.
- The ground pass samples a far-depth proxy because it renders inside the main
  depth pass; the volume, particle and shock passes sample the real scene depth
  and are occluded by buildings.
- The same-GPU-class performance comparison is pending; lavapipe benchmarks are
  functional baselines only.

## [0.10.5] - 2026-10-29

0.10.5 is a feature patch over 0.10.0: the procedural town now uses ten
different low-poly vehicles, the startup splash cycles through stills from the
promo, and the panel handle and continuous scene density are fixed. The README
was streamlined and now embeds the 0.10.0 promo as a GIF.

### Highlights

- **Ten low-poly vehicles** - sedans, sports cars, SUVs, a van, a taxi, a police
  car and two trucks from the CC0 Kenney Car Kit replace the three procedural
  car shapes.
- **Animated startup splash** - 20 stills from the first ten seconds of the
  promo, one per 5 % loading step, cropped to the splash aspect.
- **Panel and density fixes** - the menu handle now rides on the panel's left
  edge, and the scene density slider scales the object count continuously.

### Added

- **Ten low-poly vehicle models** - the procedural town now uses ten different
  CC0 vehicles from the Kenney Car Kit (sedans, sports cars, SUVs, van, taxi,
  police car and two trucks). The models are converted to per-vertex colours and
  keep their natural proportions, so the generated traffic looks varied instead
  of reusing three procedural car shapes.
- **Animated startup splash** - the splash now cycles through 20 stills taken
  every 0.5 s from the first ten seconds of the promo, one per 5 % progress
  step. The stills are cropped to the 980:640 splash aspect; the first frame is
  preloaded and the rest are decoded on demand on the splash thread.

### Fixed

- **Menu handle follows the panel** - the handle was pinned to the right window
  edge. It now sits on the panel's left edge and slides with the menu, while
  still remaining at the window edge when the panel is fully closed.
- **Scene density scales continuously** - the density slider changed the object
  budget while the per-block fill stayed quantised at `ceil(density)`, so the
  visible object count jumped at 1.0, 2.0, ... and stayed flat in between.
  Fractional slots and placement probabilities now scale the town smoothly over
  the whole slider range.

### Changed

- **README** - streamlined around the unique features and physical principles;
  the 0.10.0 promo is embedded as an optimized GIF with a full-quality MP4 link.
- **Repository hygiene** - root-level source videos and the test settings file
  are ignored; PNG assets under `assets/` remain tracked.

## [0.10.0] - 2026-10-28

0.10.0 makes the pressure wave visible in the air and ties the heat damage to
the surface that actually faces the burst. It also makes the height of burst
exact for every charge, including the high-fidelity solver domain, restores the
hotkeys after menu interaction and clears the bloom when the simulation is
rearmed.

### Highlights

- **Atmospheric shock front** - the compressed air behind the front is rendered
  as a refracting spherical shell. The Gladstone-Dale index jump from the
  Rankine-Hugoniot density ratio bends the scene in screen space, with a faint
  caustic rim and slight dispersion.
- **Directional heat damage** - only the blast-facing surface of a burning object
  glows and emits flames; short ember streaks leave the outer edges of that face.
  The leeward side keeps its charred material colour.
- **Exact height of burst** - the fireball stays at the configured height for
  every charge, and the high-fidelity solver domain is sized to contain the burst
  point, so the resolved cloud and the pressure front coincide.
- **Hotkeys and bloom** - the shortcuts work after clicking a menu control, and
  rearming or restarting clears the previous explosion's bloom.

### Added

- **Atmospheric shock front** - the pressure wave is now visible in the air as a
  refracting spherical shell. The index jump follows the Gladstone-Dale relation and
  the Rankine-Hugoniot density ratio; the scene behind the front is bent in screen
  space along the projected density gradient, with a faint caustic rim and slight
  dispersion. The pass runs before bloom, so the smoke and the sky are distorted
  together, and it uses the far side of the shell after the front has passed the
  camera.
- **Directional heat damage on objects** - burning objects no longer glow orange all
  over. Only the surface facing the burst receives the ember glow and the flame
  particles, while the leeward side keeps its charred material colour. Short ember
  streaks leave the two outer edges of the irradiated face and are blown away from
  the burst.

### Fixed

- **Height of burst no longer scales with the charge** - the fireball centre used
  to be dragged upward by the cloud rise, capped by twice the fireball radius. A
  larger charge therefore lifted the visible explosion centre even though the
  height setting was unchanged. The fireball now stays exactly at the configured
  burst height for every charge; only the mushroom cloud rises with buoyancy.
  The slider also accepts the same 0.01-5000 m range as `settings.ini`, so a
  loaded value is no longer clamped to 400 m in the interface.
- **High-fidelity bursts stay at the configured height** - the solver domain was
  sized only from the fireball radius and then clamped the charge to its ceiling.
  A 400 m burst therefore resolved its cloud around 120 m while the analytic
  shock front sat at 400 m. The domain now includes the burst point and the
  fireball above it, so the resolved cloud and the pressure front coincide.
- **Bloom no longer lingers after rearm or restart** - the bloom mip chain is
  cleared when the simulation is rearmed (`R`, Rearm button) or restarted, so the
  previous explosion's glow disappears immediately instead of staying on screen.
- **Hotkeys work after using the menu** - the shortcuts are now only suppressed
  while a text field has focus (`io.WantTextInput`) instead of whenever the
  interface captures the keyboard, so `D`, `R`, `F`, `H`, `Space` and `F12` keep
  working after clicking a menu control.

### Changed

- **CI actions** - `actions/checkout@v5` and `actions/upload-artifact@v6` run on
  Node.js 24, which removes the Node 20 deprecation annotation from the Windows
  workflow. The build, test and package steps are unchanged.

## [0.9.0] - 2026-10-28

0.9.0 focuses on the high-fidelity solver, the rendering budget and the startup
experience. The Euler plume now follows the analytic reference cloud, the mushroom head
is placed from the measured geometry, and the lighting is view-independent. The
reduced-order volume bake and the particle system moved to the GPU, buildings now crumble
under the blast wave instead of springing apart, and a splash screen covers the long
shader compilation.

### Highlights

- **View-independent high-fidelity lighting** - a precomputed sun-transmittance volume,
  fixed-step raymarching with empty-space skipping, a multiple-scattering floor and an
  isotropic 27-point shading normal.
- **Mushroom head on the analytic trajectory** - the cap closure uses the analytic
  reference cloud, the drag uses the analytic radius and a one-sided rise guide keeps the
  resolved head on the same path; the camera frames the resolved field.
- **GPU volume bake** - the reduced-order analytic field is evaluated by a compute shader
  directly into the volume texture. The 256³ bake drops from 56 ms on the thread pool to
  a single dispatch, and the 16 MB (128³) / 128 MB (256³) upload disappears.
- **GPU particles** - state, integration and the three instance batches live in
  structured buffers; the CPU only spawns and uploads the new particles, and the draws
  use `DrawInstancedIndirect`.
- **Buildings crumble with the wave** - joint failures are staggered from the blast-facing
  side, and detached chunks are loaded over the whole positive phase instead of receiving
  one explosion impulse.
- **Startup splash** - a threaded splash with the hero image, measured progress shares and
  an animated bar; the main window is only shown when loading finishes.

### Added

- **Startup splash** - a borderless splash with the hero image, version, status
  text and an animated progress bar appears immediately at launch. It runs on its
  own thread, so the bar keeps moving while the Direct3D device, the shaders,
  ImGui and the simulation are prepared. The progress shares follow the measured
  startup times (shader compilation is about 96 % of the load on the reference
  machine), so the bar traverses the full range evenly instead of jumping from
  the middle to 100 %. The main window is created hidden and shown when the
  splash closes, so no blank window is visible during loading. The darkening
  overlay is premultiplied, which removes the hard seam at its top edge.

### Fixed

- **Building chunks no longer spring apart** - a destroyed building used to break every
  overstressed joint in one frame and give each chunk a radial explosion impulse plus a
  spin around the object centre. The chunks now detach from the passing wave: joint
  failures are staggered from the blast-facing side to the far side (0.03-0.26 s) plus a
  structural delay for weakly loaded joints, and each detached chunk is loaded by the wave
  over its whole positive phase (per-chunk arrival time, triangular profile normalised to
  the blast impulse) instead of receiving one velocity kick. The support flood fill runs
  once per affected object per frame, so the staggered collapse stays cheap.

### Changed

- **View-independent high-fidelity lighting** - the solver precomputes a 64³
  sun-transmittance volume once per frame and the raymarcher samples it at every step,
  so self-shadowing no longer changes when the camera moves. A multiple-scattering floor
  keeps shadowed smoke from turning black, and the raymarcher now steps at a fixed
  world-space size (one render voxel) with conservative empty-space skipping, so grazing
  rays keep the dense layers. The shading normal uses an isotropic 27-point gradient.
- **Mushroom head follows the analytic cloud** - the cap closure now uses the analytic
  reference cloud (cap ellipsoid, cloud radius, rise speed) that also drives the camera,
  dust and particles. The entrainment drag uses the analytic radius instead of the narrow
  resolved cap, and a one-sided rise guide in the upper plume adds the upward momentum the
  coarse grid loses to numerical diffusion. The head therefore rises with the analytic
  cloud while the stem and base stay buoyancy-driven. The camera frames the resolved field
  instead of the analytic prediction.
- **Ground contact** - the initial charge is deposited with an anti-aliased sphere
  fraction instead of a binary voxel mask, and the render volume applies an isotropic
  27-point binomial filter to the soot channel instead of the axis-only 7-point cross.
  The sweep order alternates per step (X, Y, Z / Z, Y, X) with the source fused into the
  last sweep, which averages the dimensional-splitting error at no extra dispatch cost.
- **GPU volume bake** - in reduced-order mode the analytic field is now evaluated by a
  compute shader directly into the volume texture. The CPU only computes the bounds and
  field parameters, so the per-bake voxel loop and the 16 MB (128³) / 128 MB (256³)
  upload disappear; the 256³ bake drops from 56 ms on the thread pool to a single GPU
  dispatch. The CPU baker remains the fallback and still handles the 1024³ split layout.
- **GPU particles** - the particle state, integration and the three instance batches now
  live in structured buffers on the GPU. The CPU only spawns and uploads the new particles
  (a few hundred per frame); the 40 000-particle per-frame loop, the single-threaded
  `BuildBatches` and the 1.9 MB upload disappear. The draws use
  `DrawInstancedIndirect`; the CPU path remains the fallback and is used when the shader is
  unavailable. Headless captures step the GPU state with every fixed simulation step.

## [0.8.0] - 2026-10-28

This release brings updates and the changelog into the app. The app checks GitHub in the
background, downloads the Win64 zip on request, verifies it against the published SHA-256
digest and installs it with a small helper that preserves `settings.ini` and rolls back on
failure. The first start after an update shows the release notes, and a **Changelog** button
at the bottom of the panel opens the full history.

### Highlights

- **In-app updates** - background GitHub check, update banner, download with progress and
  cancel, SHA-256 verification, an optional HTTP proxy and retry actions.
- **Install and restart** - a standalone helper waits for the app to exit, extracts the
  zip with miniz, replaces the executable and assets, keeps the settings and restores the
  previous files if anything fails.
- **Verified downloads** - the SHA-256 digest is checked after the download and again by
  the installer; the helper also verifies the Authenticode signature of the new executable
  (unsigned builds are accepted with a warning, invalid signatures abort).
- **Changelog in the UI** - a What's-new modal on the first start after an update and a
  Changelog button at the bottom of the control panel, with the current release or the
  full history. The changelog text is two points larger than the rest of the UI.
- **Smaller updater** - the standalone helper dropped from 328 KB to 251 KB.

### Added

- **Update check, installer and changelog viewer** - the app checks the GitHub releases
  API in the background once per day and shows a banner when a newer release is available.
  The Win64 zip is downloaded only on request (to
  `%LOCALAPPDATA%\ExplosionSim\updates`). **Install and restart** launches a small helper
  that waits for the app to exit, replaces the executable and assets, keeps `settings.ini`
  and rolls back on failure. The first start after an update shows the current release
  notes, and a **Changelog** button at the bottom of the panel opens the full release
  history.
- **Update verification and proxy support** - the download is verified against the SHA-256
  digest published by GitHub (or a `<zip>.sha256` asset), the updater verifies it again
  before installing and checks the Authenticode signature of the new executable. An
  optional HTTP proxy can be configured in the Updates section, and failed checks/downloads
  show a retry action.
- `--check-updates` runs the update check without a window, `--no-update-check` skips it
  for one run and `--version` prints the version.
- `[update]` settings: `checkForUpdates`, `lastSeenVersion`, `skippedVersion`, `proxy` and
  `lastCheckUnix`.
- The release archive now contains `ExplosionSimUpdater.exe` and `LICENSE-miniz.txt`, and
  the packaging script writes a `.sha256` file next to the zip.

### Security

- The download is verified against the GitHub asset `digest` (or a `<zip>.sha256` asset)
  before it is finalised; the helper verifies it again before installing.
- The helper runs `WinVerifyTrust` on the extracted executable. Unsigned builds are
  accepted with a warning; invalid signatures abort the installation.
- The download host and asset name are validated, and the zip is extracted with a
  Zip-Slip check.

### Changed

- The standalone updater was reduced from 328 KB to 251 KB by dropping miniz's deflate and
  stdio paths, switching to Win32 file I/O and enabling size optimisation. It streams the
  zip and hash with bounded 64 KB buffers.
- The changelog viewer uses a font two points larger than the rest of the UI.

## [0.7.0] - 2026-10-27

This performance release cuts the cost of the two heaviest systems since 0.6.0: the
procedural town generator and the high-fidelity GPU Euler solver. The default town is
visible in ~10 ms instead of waiting for the full fracture and generates ~20 % faster;
the HF solver is ~37-41 % faster at 96³-192³ with a byte-identical rendered frame. A new
solver-only benchmark makes the solver numbers reproducible.

### Performance

Measured on the reference 8-core/16-thread CPU and an RTX 3090. Solver timings use
`--solver-benchmark` with a fixed 20 t domain and 256 steps; scene timings use the
default 20 t town and the 32k-chunk budget.

| Performance fix | Benchmark | Before | After | Gain |
|---|---|---:|---:|---:|
| Voronoi fracture micro-optimizations | Default town, 9,595 chunks | 171-188 ms | 135-147 ms | ~20-25 % |
| | 32k-chunk budget, 31,860 chunks | 866-961 ms | 739-766 ms | ~15-20 % |
| Early layout preview | Time to first visible town | 135-739 ms | ~10 ms | ~13-74x |
| HF solver P0 bundle | 96³, 256 steps | 91.6 ms | 65.7 ms | -28 % |
| | 128³, 256 steps | 205.9 ms | 144.5 ms | -30 % |
| | 192³, 256 steps | 679.5 ms | 494.6 ms | -27 % |
| HF sweep tuning | 128³, 256 steps | 144.5 ms | 121.0 ms | -16 % |
| | 192³, 256 steps | 494.6 ms | 420.7 ms | -15 % |
| Solver submission cleanup | 128³ / 192³ | - | - | <1 % (noise) |
| **Total HF solver since 0.6.0** | 96³ | 91.6 ms | ~58 ms | **-37 %** |
| | 128³ | 205.9 ms | 121.0 ms | **-41 %** |
| | 192³ | 679.5 ms | 420.7 ms | **-38 %** |

The solver submission cleanup is kept as an API-overhead reduction (four-slot constant
buffer ring, one update per sub-batch, fewer bindings); the measured CPU submission is
~1.1 ms for 256 steps, so the frame remains GPU-bound.

### Added

- **Solver-only benchmark** - `--solver-benchmark` steps the GPU solver without the scene
  or raymarcher and reports per-pass GPU times from `D3D11_QUERY_TIMESTAMP` queries plus
  the CPU submission time. It uses the non-blocking reduction path, so the numbers
  reflect interactive use instead of the readback-bound headless capture.
- **Early scene preview** - asynchronous generation publishes the intact object layout in
  ~10 ms while the Voronoi chunks are fractured in the background. The previous town stays
  visible during regeneration, the camera frames the preview, and gameplay actions are
  ignored until the full scene is ready. The preview carries a layout hash, so the
  renderer invalidates the static shadow map and chunk buffers between two different
  towns.

### Changed

- **Voronoi fracture micro-optimizations** - flat face-index polyhedron storage,
  hash-welded vertices, AABB early-outs for distant bisectors and direct face-adjacency
  construction. The default 20 t town (9,595 chunks) generates in 135-147 ms (from
  171-188 ms); the 32k-chunk budget takes 739-766 ms (from 866-961 ms).
- **High-fidelity solver P0 bundle** - the buoyancy/radiation source terms are fused into
  the Z sweep, the sweep reuses the centre cell's conserved state, `StateB` is packed as
  RG32F and the render volume updates once per frame instead of after every sub-batch. A
  step is three dispatches instead of four, and the state memory drops from 72 to 56
  bytes per voxel (about 0.94 GB instead of 1.2 GB at 256³). The Z sweep has its own
  entry point (24 -> 22 temp registers); compiling the source into the X/Y sweep cost
  12 % of the total solver time. Measured on an RTX 3090 with a fixed 20 t domain and
  256 steps: 96³ 91.6 -> 65.7 ms (-28 %), 128³ 205.9 -> 144.5 ms (-30 %), 192³ 679.5 ->
  494.6 ms (-27 %). The rendered frame is byte-identical to the previous pipeline at the
  same solver step count.
- **High-fidelity solver sweep tuning** - each sweep direction has its own entry point
  (the direction constant folds the HLLC selects and keeps the fused source out of the
  X/Y shaders), the middle MUSCL stencil is shared between the two faces, and the thread
  groups are `4x4x4` instead of `8x8x8`. Measured on an RTX 3090 with 256 steps: 128³
  144.5 -> 121.0 ms (-16 %), 192³ 494.6 -> 420.7 ms (-15 %). The rendered frame is still
  byte-identical. Branchless HLLC, `rsqrt` and reciprocal hoisting were measured and
  rejected (10 % slower or no measurable gain).
- **High-fidelity solver submission cleanup** - the constant buffer is a four-slot ring
  written once per 8-step sub-batch instead of once per dispatch, and the redundant
  `UnbindAll` after every sweep is reduced to unbinding the two state UAVs.
  `--solver-benchmark` now reports the CPU submission time separately. On the reference
  RTX 3090 the submission is ~1.1 ms for 256 steps at 128³/192³, so this is a cleanup
  without a measurable frame-time change (<1 %); it was kept because it removes redundant
  API calls and the result is byte-identical.

## [0.6.0] - 2026-10-27

This feature release turns the procedural town into a more interactive and physical
environment: the app starts armed and waits for **Detonate**, the buildings, vehicles and
trees use richer procedural materials, fire is rendered by a GPU flame shader and is
integrated with the volumetric smoke, anisotropic filtering also applies to the
procedural surfaces, and the scene generator is substantially faster.

### Highlights

- **Manual armed start** - the charge is armed on launch; **Detonate** or **D** starts
  the explosion. **H** cycles panel -> overlays -> restore, and the animated right-edge
  handle collapses/reopens the panel.
- **GPU fire and smoke** - `PSFire` renders turbulent flames in one instanced draw call.
  Flames spawn on the visible surfaces, render before the volume, are occluded and
  scattered by the smoke, and are shown in both render modes. The smoke casts volumetric
  shadows and self-shadows towards the sun.
- **Richer materials and anisotropic filtering** - four facade styles, vehicle
  paint/glass/lights/tires and tree bark/foliage detail; the aniso setting filters the
  procedural surface grain and supersamples facade patterns along the major footprint
  axis.
- **Chunk controls** - a continuous 0.25x..2x chunk-size slider, static chunk colours and
  facade coordinates, and a measured **32,000-chunk** global budget.
- **Much faster scene generation** - placement is serial, the Voronoi fracture runs on
  the thread pool, and Rearm/Reset/loop reuse the town. The default 20 t town (9,595
  chunks) generates in ~0.18 s; the extreme setting is capped at 31,860 chunks (~0.9 s).

### Added

- **Parallel scene generation** - the deterministic placement pass stays serial, while
  the Voronoi fracture runs on the thread pool with one RNG per building. The default
  20 t town (9,595 chunks) generates in ~0.18 s instead of ~1.15 s.
- **Global chunk budget** - the chunk-size slider is capped at a measured **32,000
  chunks** for the whole scene. Reference timings: 16k -> 0.35 s, 24k -> 0.7 s,
  32k -> 0.9 s, 40k -> 1.2 s.
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

- Rearm, Reset, loop restarts and non-scene setting changes now reuse the generated town;
  only the mutable object/chunk state (damage, burning, bodies, joints) is reset. A new
  town is generated only when the layout settings or the charge/environment change.
- The scene hash now includes the chunk geometry, so the renderer rebuilds its chunk
  vertex/index buffers when the chunk scale or budget changes.
- Chunk texturing is static: each chunk keeps its building's colour and facade
  coordinates, and only the semi-transparent fire effects overlay it.
- The Pause/Rearm/Frame row at the top of the panel was removed. Pause and Frame now sit
  below the main Detonate/Rearm button in the same style.

### Fixed

- Ground-reflection changes now regenerate the scene, so the stored shielding factors stay
  consistent with the setting.
- Burning trees and vehicles now show flames in both render modes. Fire is spawned on
  the visible canopy rim or roof surface instead of inside the mesh, where the soft depth
  fade hid it, and the spawn point follows the current rigid-body pose of thrown objects.
  The high-fidelity mode no longer drops the scene fire batch.
- Fire is now integrated with the smoke: the flame pass is drawn before the volume, so the
  smoke's premultiplied transmittance occludes the flames and the volume's scattering
  composites over them. The smoke cloud also casts a volumetric shadow on the ground and
  scene, and the volume self-shadows towards the sun.
- A near-camera fade on the volume and on the fire billboards prevents the camera from
  picking up a screen-filling red veil when flying through the smoke or a flame.
- Flame turbulence is sampled in world space, so the pattern no longer swims with the
  camera when it moves.

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

[Unreleased]: https://github.com/Grownz/ExplosionSim/compare/v0.10.5...HEAD
[0.10.5]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.10.5
[0.10.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.10.0
[0.9.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.9.0
[0.8.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.8.0
[0.7.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.7.0
[0.6.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.6.0
[0.5.5]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.5.5
[0.5.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.5.0
[0.4.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.4.0
[0.3.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.3.0
[0.2.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.2.0
[0.1.0]: https://github.com/Grownz/ExplosionSim/releases/tag/v0.1.0
