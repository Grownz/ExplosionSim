# Roadmap: closing the remaining limitations

This document is a plan, not a description of implemented behaviour. It lists every
limitation reported for the procedural environment, the chosen solution, the rejected
alternatives and the acceptance criteria. The guiding rule for every item is: **as
physically correct as the budget allows, and fast enough to stay above 60 fps at
1920x1080 with `scene.density = 3`**.

Target releases: `0.8.0` (current), `0.9.0` (phase 6), `1.0.0` (future).

## Status (2026-10-25)

**Update:** the in-house rigid-body solver has been replaced by Jolt Physics 5.6.0
(MIT). Items 1.1-1.5 are now covered by the engine (BVH broadphase, clipped manifolds,
warm starting, box inertia, island sleeping, multithreaded solving). The measurements and
findings below describe the previous solver and are kept for history.

- **1.1 Broadphase - done.** Uniform grid with 4 m cells, AABB insertion and a
  sorted, de-duplicated pair list. At `scene.density = 3` the simulation cost per
  1/120 s step dropped from ~13.7 ms to ~5.8 ms.
- **1.4 Inertia tensor - done.** Analytic box inertia; both ground and pair
  contacts now use the world-space inverse inertia, so long chunks tumble instead
  of rotating like cubes.
- **1.5 Sleeping - unchanged.** The existing per-body sleep is kept. Island
  sleeping was implemented but reverted together with the solver rewrite below.
- **1.2 / 1.3 - shock propagation shipped; clipped manifold and block LCP not.**
  The solver keeps the existing vertex-in-box manifold and sequential impulses.
  A second **shock-propagation pass** was added: after the normal solve, the
  contacts are visited from the ground up and each upper body is resolved against
  the lower one as if it were infinitely massive. The support impulse therefore
  reaches the top of a stack within one sweep.

### Shock propagation measurements (2026-10-25)

- **Stack:** an eight-box tower now stays together for ~3.5 s (previously ~1.5 s)
  and sinks only ~5 cm. Beyond that an accumulated lateral drift and a slow tilt
  make it topple; the test asserts the stable 2.5 s window.
- **Scene:** neutral. With a fixed seed (`--scene-seed 1337`, 20 t) the awake
  count is 1472 vs 1475 bodies with the pass disabled; rubble still does not
  settle, which is pre-existing.
- **Cost:** density 3 ~7.0 ms vs 5.8 ms per 1/120 s step (three shock passes,
  ~20 %). Density 1 stays well inside the frame budget.
- **Variants tried:** per-point vs centroid application, absolute vs relative
  target, with and without shock friction, 1/2/3/6 passes. Per-point with an
  absolute target was the most stable; centroid application and shock friction
  introduced rocking, the relative target introduced lateral drift.

### Findings (kept for the next attempt)

- The LCP right-hand side must include `A * lambda_old`; omitting it makes warm
  starting inject energy instead of removing it.
- Four coplanar contact points make the normal system rank-deficient; a small
  diagonal regularisation selects the symmetric impulse distribution.
- Three points are statically determinate but form a right triangle on a square
  face, so the unique impulse distribution is asymmetric and induces a slow tilt.
- Two points leave a box free to rock about the connecting line.
- The previous solver only looked stable because touching boxes produced a single
  centroid contact (no contact torque).
- The remaining bottleneck is chain convergence and the lateral drift that
  accumulates in tall stacks. A future solver should combine shock propagation
  with a clipped manifold and graph-coloured batching, or switch to XPBD.

## Status: phases 2-4 (2026-10-25)

- **2.1 Progressive collapse - done.** Per-edge joints with staggered failure times and a
  pending-failure list. Each failure re-runs the support flood fill, so chunks detach in
  waves instead of all at once. Test: a destroyed building goes from 243 to 513 detached
  chunks over the second after the blast.
- **2.2 Static structure - done.** Chunks that are still standing become static collision
  boxes; detached chunks reuse the same body and are switched to dynamic. Contact impulses
  above 500 N s are converted to pressure and damage the struck chunk's joints.
- **2.3 Jittered chunks - done.** Split planes are offset by up to +-28 % per axis, so no
  two neighbours share dimensions; topology and OBB physics are unchanged.
- **2.3 phase B (Voronoi) - default.** Convex cells clipped to the building box,
  flat-shaded hulls, principal-component fitted OBB proxies and face-based joints. The
  jittered grid remains available through *Environment > Voronoi chunks*
  (`scene.voronoiChunks`). Measured at density 1 (1920x1080, 20 t): 4.48 ms/frame versus
  4.28 ms for the grid.
- **3 Blast shielding - done.** Nine rays per receiver, thickness-based optical depth,
  diffraction floor and an optional ground-reflection setting (default off).
- **4 Shadows and LOD - done.** Three cascades (2048/1024/1024) with practical splits,
  slope-scaled normal offset, 5x5 PCF, 10 % blending into the next cascade and a
  backwards-extended light frustum for the blend band. Intact buildings are cached in a
  camera-independent world-space 4096^2 shadow map (re-rendered on scene, sun or collapse
  changes); the dynamic cascades contain the rubble, vehicles and trees in every cascade,
  so the rubble casts dynamic shadows on the ground, the intact buildings and other
  debris. Chunk casters are kept up to `max(200 m, 2 * scene extent)` outside the camera
  frustum, matching the static building map's coverage. A single-cone tree mesh beyond
  80 m and the facade fade.

Measured at density 1 with a 20 t charge and seed 1337: 3553 bodies (864 static),
render 2.2 ms/frame at 1920x1080, physics 2.8 ms per 1/120 s step. At density 3:
10681 bodies (2592 static), render 5.2 ms, physics 8.6 ms per step.

## 0. Baseline and budget

Measured with `--benchmark --frames 120 --reduced-order --time 1.5 --charge 20000`,
1920x1080, seed 1344, `scene.quality = 2`:

| Density | Objects (B/V/T) | Chunks | Bodies | Render | Sim per 1/120 s | 60 fps frame |
| ------- | --------------- | ------ | ------ | ------ | --------------- | ------------ |
| 1       | 124 / 59 / 63   | 4 464  | 1 453  | 1.4 ms | ~3.8 ms         | ~9 ms        |
| 3       | 335 / 192 / 220 | 12 060 | 2 769  | 3.2 ms | ~13.0 ms        | ~29 ms       |

The frame budget is 16.7 ms. Planned split at density 3: volume bake 3 ms, render 4 ms,
physics 4 ms, particles/UI 2 ms, headroom 3 ms.

Every phase ends with a benchmark run and a short entry in `CHANGELOG.md`.

## Phase 1 - physics core (limitations: O(n^2) broadphase, weak manifolds, isotropic inertia)

### 1.1 Broadphase: uniform grid plus a static list

**Now:** `PhysicsWorld::Step` tests every pair (`src/scene/PhysicsWorld.cpp:205`), so the
cost grows with n^2. At density 3 this dominates the frame.

**Solution:**

- Hash grid over all three axes with 2 m cells (2x the median chunk half-extent).
  Rebuild every substep from the body AABBs.
- Allocation-free layout: a power-of-two bucket table (8192 entries), `nextInCell`
  indices and a per-body cell coordinate. No `std::unordered_map` in the hot loop.
- Pair generation: for each body, walk its own cell and the 13 forward neighbours (3D
  half-neighbourhood) and emit `(min,max)` index pairs; deduplicate by sorting the pair
  keys, which also makes the solver order deterministic.
- Static bodies (standing building chunks, phase 2.2) live in a separate array and are
  only tested against moving bodies whose AABB overlaps a per-building AABB. There are at
  most a few hundred of them, so a linear pass is cheaper than a tree.
- Large bodies (trees up to 12 m tall, whole-facade chunks up to 8 m) are inserted into
  every cell their AABB overlaps.

**Rejected:** sweep-and-prune (poor with many resting bodies and vertical piles), dynamic
AABB tree (more code, harder to keep deterministic, overkill for 3k bodies).

**Budget:** 3k bodies, ~8k pairs, target <= 0.4 ms per substep.
**Acceptance:** `--profile-physics` reports broadphase <= 0.4 ms at density 3; identical
pair sets for identical inputs across runs.

### 1.2 Narrowphase: clipped contact manifold

**Now:** only vertices of one box inside the other are used, with a midpoint fallback for
edge-edge (`BuildManifold`, `src/scene/PhysicsWorld.cpp:100`). Face contacts can lose
points, edge contacts are one point at an arbitrary midpoint.

**Solution:** standard box-box clipping (Erin Catto style):

1. 15-axis SAT keeps the axis with the minimum overlap and classifies it as face-A,
   face-B or edge-edge.
2. Face case: the winning face is the reference face; the most anti-parallel face of the
   other box is the incident face. Clip the incident polygon against the four side planes
   of the reference face (Sutherland-Hodgman), keep points behind the reference face, then
   reduce to at most four points spread over the contact area.
3. Edge case: closest points between the two supporting edges, one contact point.
4. Store a `featureId` per point (vertex index or clipped-edge id) so warm starting can
   match points between frames.

**Rejected:** full GJK/EPA (correct for arbitrary convex hulls, but boxes are exact with
SAT and much faster); per-triangle collision (not needed, chunks are convex).

**Budget:** ~8k pairs, target <= 0.6 ms per substep.
**Acceptance:** a 4-point face contact is produced for two stacked boxes; an edge contact
produces one point at the true closest point (unit test with analytic expectation).

### 1.3 Solver: warm starting, friction cone, split impulse

**Now:** 6 iterations, impulses are recomputed from scratch each iteration, friction is
clamped per iteration against the current normal impulse, no accumulation, no position
slop. This causes jitter and slow sinking in piles.

**Solution:**

- Persistent manifolds in a pair-keyed cache (`std::vector` plus an open-addressing hash
  of `uint64` keys). Points are matched by `featureId`, then by proximity (< 2 cm).
- Accumulated normal impulse and two accumulated tangent impulses per point. Warm start
  from the cache before the iteration loop; zero unmatched points.
- Iterations: 8. Friction: cone `|jt| <= mu * jn` with the two tangents solved together
  (static friction), not per-iteration clamping.
- Restitution only when the approach speed is below -1 m/s (prevents micro-bouncing).
- Position correction: split impulse (pseudo-velocity), slop 5 mm, max correction
  3 m/s. This removes the energy injection of plain Baumgarte.
- Ground contacts use the same solver with a static body (`invMass = 0`) instead of the
  special-case code in `ResolveGround`.

**Rejected:** block solver for 2-point contacts (helps tall stacks, but the scene has
mostly rubble piles; can be added later behind a flag); XPBD (different feel, larger
rewrite).

**Budget:** <= 1.2 ms per substep at 8k contacts.
**Acceptance:** a 20-box pyramid is stable for 5 s with < 2 cm drift; a box on a slope
slides only above the friction angle; no penetration deeper than the slop after settling.

### 1.4 Inertia tensor and multi-point ground contact

**Now:** isotropic inertia from the bounding sphere (`AddBody`,
`src/scene/PhysicsWorld.cpp:162`) and one averaged ground contact point
(`ResolveGround`), so boxes tip unrealistically.

**Solution:**

- Store the diagonal body-space inverse inertia for a box:
  `I = m/12 * diag(h^2+d^2, w^2+d^2, w^2+h^2)` with full extents `w,h,d`.
- Per substep build `Iinv_world = R * diag(invI) * R^T` once per body and reuse it for
  every contact: `dw = Iinv_world * (r x J)`.
- Ground contact: generate up to four contacts at the deepest corners (separation
  > -2 cm) and feed them through the normal solver; friction per point.
- Keep the gyroscopic term out of the integration (standard for game physics; document
  it).

**Budget:** ~30 flops per body per substep, negligible.
**Acceptance:** a tall box pushed at the top topples instead of sliding; a box dropped on
a corner settles on a face; a sliding box decelerates at `mu*g` within 5%.

### 1.5 Island sleeping

**Now:** each body sleeps on its own; a pile never sleeps as a whole and keeps waking
neighbours.

**Solution:** union-find over the contact graph each substep (static bodies do not merge
islands). An island sleeps when every body has been below the thresholds for 0.7 s;
waking one body wakes the island. This also shrinks the active pair set in settled
rubble.

**Acceptance:** after a collapse, the rubble pile reaches 0 active bodies; waking one
chunk re-activates only its island.

## Phase 2 - progressive collapse and fracture

### 2.1 Joint health and event-driven collapse

**Now:** `CollapseBuilding` runs once per building and detaches everything unsupported at
that instant; the remaining structure is frozen.

**Solution:**

- Per chunk edge a `health` in [0,1]. The blast load sets an initial joint stress; the
  failure time is `arrival + k * (1 - health)` so joints fail in a staggered sequence.
- A priority queue of pending failures. On failure: break the edge, detach the chunk,
  add a rigid body, and re-run a **local** flood fill from the affected chunks (bounded
  to the building) to find newly unsupported chunks.
- Detached chunks inherit the blast velocity plus a small delay, so the collapse front
  moves downward/outward like a real progressive failure.

**Acceptance:** a 4-storey building hit at the base loses the ground floor first, then
the upper floors in height order; the failure timestamps are monotonic with height.

### 2.2 Standing structure as static colliders

**Now:** falling debris passes through the remaining building.

**Solution:** register every still-attached chunk of a damaged building as a static box
(`invMass = 0`, `static = true`) in the physics world. Debris collides with it. On a
contact impulse above a threshold, convert it to an equivalent pressure
(`J / (A * dt)`) and subtract it from the joint health of the hit chunk, so debris can
knock out lower floors and trigger a cascade.

**Budget:** a few hundred static boxes; handled by the static list in 1.1.
**Acceptance:** debris piles up against a standing wall; a 2 t chunk dropped from 20 m
breaks a ground-floor joint.

### 2.3 Chunk geometry

**Phase A (recommended, cheap):** jittered split planes. The grid split positions are
offset deterministically per building and axis, so chunks have different sizes and no
two neighbours share dimensions. Topology and OBB physics stay unchanged, rendering stays
instanced. Add +-3 % colour variation.

**Phase B (optional, if visual fidelity becomes the priority):** Voronoi fracture clipped
to the building box, one convex hull per cell. Physics via GJK/EPA (or an OBB
approximation), rendering via one vertex/index buffer per building with per-chunk index
ranges (loses instancing, costs memory). Only worth it if close-up rubble is a goal.

**Status:** Phase B is the default. Phase A remains available by disabling *Environment >
Voronoi chunks* (`scene.voronoiChunks`). Phase B clips cells against each other's
bisector planes, uses a PCA-fitted OBB (no GJK/EPA) as the collision proxy, and draws the
hulls with one non-instanced draw call per building, with the chunk id stored per vertex.
The support graph uses shared Voronoi faces instead of the six grid neighbours.

**Acceptance (A):** no two adjacent chunks share identical dimensions; the support graph
keeps six neighbours; render cost unchanged.

## Phase 3 - blast shielding

**Now:** one segment from ground zero to the object centre against building AABBs, binary
occluder count, `0.35^n` (`src/scene/Scene.cpp:386`).

**Solution:**

1. **Multi-ray transmission.** Sample the receiver box on a 3x3 grid (base/mid/top,
   left/centre/right) and average the per-ray transmission. Each ray finds the nearest
   occluder and its entry/exit distance (thickness) and uses
   `T_ray = exp(-thickness / lambda_s)` with a structural screening length
   `lambda_s ~ 3 m`, clamped to [0.05, 1]. This captures partial occlusion (a building
   covering the lower half only).
2. **Single-edge diffraction.** For blocked rays compute the Fresnel parameter
   `v = h * sqrt(2/lambda * (1/d1 + 1/d2))` with `lambda = c * T_pos` (positive-phase
   duration), `h` = distance from the receiver to the shadow boundary, and the ITU-R
   P.526 loss `L(v) = 6.9 + 20 log10(sqrt((v-0.1)^2+1) + v - 0.1)` for `v > -0.78`.
   The final transmission is `max(T_shadow, 10^(-L/20))`, because the diffracted wave
   dominates inside the shadow.
3. **Ground reflection.** Image source at `(0, -HoB, 0)`, reflection coefficient 0.9;
   combine the direct and reflected overpressure with the path-difference phase. Expose
   `blast.groundReflection` (0 = free air, default 0.9). Keep the free-air case
   bit-identical for existing tests.
4. **Small occluders.** Trees attenuate a ray that passes the canopy by 0.85, vehicles by
   0.95. Documented as plausibility factors, not validated values.

**Cost:** 9 rays x ~20 occluders x ~250 objects = 45k AABB tests, once at generation.
**Acceptance:** an object in the lee of a building receives less load than the same
object in the open at the same distance; a receiver half-covered by an edge gets a value
between the two; free-air results are unchanged.

## Phase 4 - shadows and LOD

**Now:** one 2048x2048 cascade over the whole town, fixed NDC bias 0.0015, 3x3 PCF,
camera-frustum caster culling.

**Solution:**

1. **Three cascades** (2048 / 1024 / 1024) split with the practical scheme
   (`lambda = 0.7`) at 40 m and 120 m, plus a far cascade to 400 m. Cull casters against
   each cascade's light frustum, not the camera frustum. Blend the last 10 % of each
   cascade.
2. **Bias:** slope-scaled depth bias plus a normal offset of two texels instead of the
   fixed 0.0015 NDC value. Removes peter-panning and acne on grazing surfaces.
3. **Filtering:** 5x5 PCF with a rotated Poisson disk. Optional EVSM later if softer
   contact shadows are wanted.
4. **Static caster cache** (only if the shadow pass exceeds 1.5 ms): the sun and the
   intact buildings are static, so render them into a cached map per cascade once per
   settings change; render only moving debris every frame and take the minimum depth in
   the shader.
5. **LOD:** three-level vegetation chain (full cone+trunk, single cone, billboard cross)
   and a two-level vehicle chain, selected by distance and `scene.quality`, with 10 %
   hysteresis. The far shadow cascade renders buildings only.

**Note on ground shadows:** the "no shadow from the ground onto objects" limitation is a
non-issue for an infinite flat plane; it cannot occlude anything above it. It is dropped
from the list rather than implemented.

**Rejected:** HZB occlusion culling. The town is open and the large occluders are already
culled by the frustum; the extra pass would cost more than it saves. Documented as an
accepted limitation.

**Acceptance:** <= 5 cm shadow texel at 100 m; no visible cascade seam; density 3 stays
above 60 fps at 1080p.

## Phase 5 - fire, wind and soil

### 5.1 Fire spread

**Now:** burning is a visual flag; fire does not propagate.

**Solution:** per-object fuel (building 20-60 kg/m^2, vehicle 40 kg, tree 15 kg), burn
rate proportional to the exposed surface, burning duration = fuel / burn rate. Ignition
by a burning neighbour uses a radiative term `q = sigma * eps * T^4 * viewFactor` with
`viewFactor ~ 1/(1+d^2)` and a 30 m cutoff, evaluated every 0.5 s. Emission of embers and
smoke scales with the burn rate; charring grows with the burned fraction. No chemistry,
no flame simulation.

**Acceptance:** a centre fire spreads to adjacent buildings within ~30 s in still air and
stops at a 20 m gap; the burn duration scales with the fuel load.

### 5.2 Wind on debris

**Solution:** aerodynamic drag `F = 0.5 * rho * Cd * A * |v_rel| * v_rel` with the
cross-section `A` from the OBB, `Cd = 1.0`, the log-law wind profile at the body height,
applied at the centre of pressure (offset from the centre of mass, so it also produces a
torque). `rho` comes from the atmosphere model.

**Acceptance:** a 0.2 kg splinter drifts visibly downwind, a 10 kg chunk barely deviates.

### 5.3 Crater heightfield

**Solution:** analytic crater profile
`y(r) = -depth * exp(-(r/R)^2) + 0.15 * depth * exp(-((r-R)/(0.3R))^2)`, with `depth` and
`R` from the charge energy and height of burst. The physics ground contact samples this
heightfield (bilinear) instead of `y = 0`, and the ground shader uses the same analytic
profile. Debris rolls into the crater.

**Acceptance:** the crater volume scales with the charge energy; debris comes to rest
inside the crater rather than on a flat plane.

## Phase 6 - scope and determinism

### 6.1 People

Recommendation: keep excluded. It is a product/scope decision, not a technical gap. If it
is ever required: capsule proxies with a two-segment ragdoll and P-I injury thresholds,
no gore. ~2-3 days.

### 6.2 Determinism

- Contract: bit-exact for the same binary on the same platform; not across compilers or
  architectures.
- Remove `/fp:fast` from the physics translation units (or use `/fp:precise`) to avoid
  FMA contraction differences.
- If cross-platform replay is required, replace libm `sin/cos` in the hot simulation path
  with deterministic polynomial approximations.
- Add a state-hash test: run 600 fixed substeps and hash positions/orientations. Store the
  golden value per platform and fail on change.

## Cross-cutting work

- **Profiling harness:** `--profile-physics` runs N substeps and reports broadphase,
  narrowphase, solver, island and sleep timings via QPC. Extend `Scene::Stats()` with
  active/sleeping bodies, contacts and islands.
- **CI:** keep `physics` and `gpu` tests green; add a soak test (density 3, 600 steps) with
  a soft timing assertion.
- **Docs:** update `docs/physics.md` and `docs/architecture.md` per phase; add a
  "validated against" note for every empirical constant (friction, screening length,
  reflection coefficient).

## Priority and effort

| Phase | Items | Impact | Effort | Risk |
| ----- | ----- | ------ | ------ | ---- |
| 1.1 | Broadphase | 60 fps at density 3 | 1-2 d | low |
| 1.2-1.3 | Manifold, warm start, split impulse | stacking quality | 3-4 d | medium |
| 1.4 | Inertia, ground contacts | tumbling realism | 1 d | low |
| 1.5 | Island sleeping | performance | 0.5 d | low |
| 2.1 | Joint health, cascade | collapse realism | 2 d | medium |
| 2.2 | Static structure colliders | debris interaction | 1.5 d | medium |
| 2.3 | Jittered chunks | visual variety | 0.5 d | low |
| 3 | Shielding, diffraction, reflection | blast correctness | 2 d | medium |
| 4 | CSM, bias, LOD | shadow quality | 3 d | medium |
| 5 | Fire spread, wind, crater | environment fidelity | 4 d | medium |
| 6 | Determinism, docs | robustness | 1 d | low |

Recommended order: 1.1 -> 1.2/1.3 -> 1.4 -> 1.5 -> 2.1 -> 2.2 -> 2.3 -> 4 -> 3 -> 5 -> 6.
Broadphase first unblocks the density benchmark; phase 2 depends on the solver from
phase 1.

## Accepted limitations (documented, not planned)

- No compressible CFD combustion; fire remains a radiative spread model.
- No soil plasticity, only an analytic crater heightfield.
- No HZB occlusion culling (open town, low payoff).
- Cross-platform bit-exact replay is not guaranteed.
- People remain out of scope.
