# Physics model

This document describes the reduced-order explosion model used by the simulator. All
internal calculations are performed in SI units (metres, kilograms, seconds, kelvin,
pascal, joule).

The goal is a model that is physically consistent, parameterised by real quantities and
fast enough for interactive rendering. It is **not** a compressible CFD solver: the shock
front is integrated from an empirical overpressure fit and the fireball/cloud are
represented by analytic fields. This is the same class of model used for real-time
visualisation and preliminary hazard assessment; its limitations are listed at the end.

## 1. Charge and energy

A charge is described by its TNT-equivalent mass `W` in kilograms and its height of burst
`h`. The detonation energy is

```
E = W · 4.184 MJ/kg
```

The corresponding explosive volume is computed with the density of TNT
(`ρ_TNT = 1630 kg/m³`), giving an initial charge radius
`R_0 = (3W / (4π ρ_TNT))^(1/3)`.

## 2. Blast scaling

Distances are non-dimensionalised with the Hopkinson-Cranz cube-root scaling

```
Z = R / W^(1/3)          [m/kg^(1/3)]
```

and corrected for ambient pressure with the Sachs scaling

```
Z_eff = Z · (p0 / p_ref)^(1/3)
```

so a charge at altitude produces a larger fireball and a lower absolute overpressure at
a fixed physical range. The ambient density (including humidity) and the speed of sound enter the shock
integration and the particle drag.

### Moist atmosphere

The air density uses the partial pressures of dry air and water vapour:

```
p_sat = 610.94 · exp(17.625 · T_C / (T_C + 243.04))     [Pa]
p_v   = RH · p_sat
rho   = (p - p_v) / (R_d T) + p_v / (R_v T)             [kg/m³]
c     = sqrt(gamma · p / rho)                            [m/s]
```

with `R_d = 287.058`, `R_v = 461.5` J/(kg K). Humidity therefore lowers the density
and slightly raises the speed of sound. The high-fidelity solver uses the resulting
virtual temperature so its ideal-gas equation of state remains consistent.

The peak incident overpressure follows Brode's two-branch fit:

```
Z < 0.85 :  Δp = 6.7 / Z³ + 1                         [bar]
Z ≥ 1.00 :  Δp = 0.975/Z + 1.455/Z² + 5.85/Z³ - 0.019  [bar]
```

The two branches are blended with a smoothstep over `Z = 0.85 … 1.0` instead of being
switched discontinuously. The blended curve is continuous and monotonically decreasing,
which the inverse radius solver relies on. The fit is evaluated in bar and converted to
pascal. The negative offset of the far-field branch makes it cross zero near `Z ≈ 55`;
beyond that point the model continues with an acoustic `1/r` tail so the shock front does
not pin to ambient pressure.

### Shock velocity

The local shock Mach number follows from the Rankine-Hugoniot relations:

```
M = sqrt(1 + (γ + 1) / (2γ) · Δp / p₀)
U = M · c₀
```

with `γ = 1.4`, `p₀ = 101 325 Pa` and `c₀ = sqrt(γ R_s T₀) ≈ 340 m/s`.

The shock radius is integrated with a second-order (midpoint) scheme:

```
dR/dt = U(Δp(Z(R))),   R(0) = R_0
```

The step is limited by a CFL-like condition (`0.08 · R / U`, clamped to 20 µs … 5 ms),
which keeps the integration stable from the near field (kilometres per second) to the
acoustic far field. Every step is recorded in a monotonic `(t, R)` table used for
observer arrival times.

### Observer waveform

The pressure history at a fixed distance is modelled with the Friedlander waveform

```
p(t) = Δp · (1 - τ/t₊) · exp(-b τ/t₊),   τ = t - t_arrival
b = clamp(0.9 + 2.2 / (1 + 0.45 Z), 0.9, 3.0)
```

where the positive-phase duration is estimated from the shock transit time,
`t₊ ≈ 1.1 · R / U` (clamped to 5 s). The decay coefficient shrinks with scaled distance,
so far-field waveforms keep their longer tail instead of being over-damped by a fixed
`b = 1.8`. This remains a qualitative approximation; published fitted durations differ by
a factor of order one.

## 3. Fireball

The fireball radius grows as a saturating Sedov-like law:

```
R_fb(t) = R_max · (1 - exp(-t/τ_g))^0.4
```

The growth time scale is `τ_g = 0.05 · W_kt^0.25 + 0.004 s`. The maximum radius blends
two empirical regimes:

```
R_max = max(50 · W_kt^0.4, 0.9 · W_kg^0.25)     [m]
```

The first term is the nuclear fireball scaling, the second dominates for small
conventional charges. The visible surface temperature is held at 3600 K during formation
(about four growth time constants) and then cools exponentially:

```
T(t) = T_amb + (T_surf - T_amb) · exp(-(t - t_f)/τ_c)
τ_c = max(1.2 · W_kt^0.15, 0.75) s
```

The volume field uses a hot core (up to ~1.4 × T_surf) and a cooler, soot-rich surface,
so the blackbody colour ranges from yellow-white in the core to deep orange-red at the
rim. A soot shell forms as the surface cools and absorbs the emission.

The emitted colour is computed from Planck's law via the Planckian locus approximation
(Kim et al., 2002), converted to linear sRGB. Emission is integrated along the view ray
together with absorption, so the fireball shows limb darkening and is occluded by its own
smoke.

## 4. Mushroom cloud

A fraction `f_heat = 0.12` of the yield remains as thermal energy in the rising cloud. The
initial temperature excess is

```
ΔT₀ = f_heat · E / (m_cloud · c_p),   m_cloud = ρ₀ · (4/3)π R_max³
```

The cloud is treated as a buoyant thermal with entrainment:

```
R_c(t)  grows with 0.45 · v_rise + 0.10 · R_max
ΔT(t)   = ΔT₀ · (R_max / R_c)³
dv/dt   = g · ΔT/T₀ - 0.6 v|v| / (2 R_c)
```

The computed rise height, cloud radius and wind drift feed the volumetric field (a torus
for the cap and a tapered column for the stem). For a 200 g charge the temperature excess
is only a few kelvin, so the cloud stays low and compact; for kiloton-scale charges it
rises hundreds of metres.

## 5. Thermal radiation

For TNT, roughly 5 % of the yield is emitted thermally. The radiant exposure at a slant
range `r` is

```
Q = 0.05 · E · 0.85 / (4π r²)      [J/m²]
```

which is inverted to obtain the third-degree burn radius (80 kJ/m²). Atmospheric
attenuation is represented by the constant transmission factor 0.85.

## 6. Debris and particles

Fragments receive a Gurney-type velocity distribution (several hundred metres per second
for a cased charge), then follow ballistic trajectories with quadratic drag:

```
a = -0.5 · ρ_air · C_d · A / m · |v| v + g
```

with ground bounce (restitution 0.22, tangential damping 0.55) and radiative cooling.
Sparks use a larger drag coefficient and short lifetimes. Smoke and dust are buoyant
particles with wind advection, growth and distance-based opacity. Particle sizes, counts
and lifetimes scale with the fireball radius so that the same parameters work from grams
to tonnes.

## 7. Volume field
The renderer consumes a 3D field with

- `R` – extinction density,
- `G` – temperature / 12 000 K,
- `B` – dust fraction (albedo selector),
- `A` – soot fraction (dark, absorbing).

The fireball is a sphere with a noise-perturbed radius, a hot core and a filamentary
temperature field. A soot shell forms on the cooling surface, and a turbulent smoke
envelope grows around the fireball and progressively hides it. A thick dust skirt is
lifted at the base by the shock; it is lit by the fireball and later obscures the lower
half of the cooling fireball. The shock front is a thin shell with the post-shock
temperature ratio

```
T₂/T₁ = [(2γM² - (γ-1))((γ-1)M² + 2)] / [(γ+1)² M²]
```

the cap is a torus, the stem a tapered column, and the ground ring an expanding annulus.

## 8. High-fidelity GPU solver

The optional high-fidelity mode replaces the analytic volume with a finite-volume
solution of the 3D compressible Euler equations:

```
dU/dt + dF/dx + dG/dy + dH/dz = S
U = (rho, rho*u, rho*v, rho*w, E, rho*Y)
```

with the ideal-gas equation of state `p = (gamma - 1)(E - 0.5 rho |v|^2)` and
`T = p / (rho R_s)`.

### Numerical scheme

- **Reconstruction** – MUSCL with a minmod limiter on the primitive variables
  `(rho, u, v, w, p, Y)`; the reconstruction falls back to the cell-centred state if the
  density or pressure becomes non-positive.
- **Riemann solver** – HLLC with the Davis wave-speed estimates
  `S_L = min(u_L - c_L, u_R - c_R)`, `S_R = max(u_L + c_L, u_R + c_R)`. A Rusanov flux is
  used if the star region is degenerate.
- **Splitting** – three directional sweeps followed by a source pass per time step, run
  in sub-batches of 8 steps.
- **Time step** – CFL-limited with `dt = 0.20 dx / max(|v| + c)`. The maximum is computed
  on the GPU; interactive runs poll it without blocking, captures wait for it.

### Initial condition and boundaries

The charge is deposited as a sphere of radius `max(R_charge, 3.5 dx)` with the TNT mass
and energy, and `Y = 1` (soot density equal to the local gas density). For low bursts the
part of the sphere below the ground is not present in the grid; the density and energy
density are scaled by the ratio of the full sphere volume to the above-ground volume, so
the total charge mass and energy are preserved instead of being truncated. The ground is a
reflecting wall (mirrored ghost cell with reversed vertical momentum). The lateral and
top boundaries use zero-gradient ghost cells plus an absorbing sponge over the outer 6 %
of the domain that relaxes the state towards ambient, so outgoing waves do not reflect
back.

### Source terms

- Buoyancy uses the density difference relative to the ambient air, so the uniform
  ambient atmosphere does not accelerate.
- A soot cooling term removes energy from hot products.
- The render conversion applies a sub-grid **soot incandescence** temperature floor. The
  raymarcher adds gradient-based sun shading from the density field. The temperature floor
  represents the optical thickness and afterburning of the products, which the adiabatic
  Euler solution does not capture.

### Limitations

- The grid is Cartesian and uniform; 128³ is a compromise between resolution and
  real-time performance. 256³ and 512³ are available for stills or very slow motion
  (the four RGBA32F state volumes plus the render volume need about 1.2 GB at 256³ and
  9.7 GB at 512³; 1024³ would need about 76 GB and is therefore not offered).
- Auto-scale expands the domain in discrete 1.3× steps and resamples the state, which
  slightly diffuses the shock and contact surface at each expansion. The analytic cloud
  drives the growth timing at 70 % of the domain; the measured product bounds are a hard
  safety net at 95 %, and the radial measurement ignores the base surge below 25 % of the
  domain height. Up to an accuracy factor of 16 is allowed.
- The solver runs in a still-air frame. Wind is represented by translating the rendered
  volume downwind with the solver time, consistent with the reduced-order cloud drift.
- Numerical diffusion smears the contact surface and the soot over time.
- Chemical afterburning is not solved; it is represented by the render-side temperature
  floor.
- The mode is intentionally slow-motion: 24 steps per frame advance a fraction of a
  millisecond of physical time.

## 9. Limitations

- The shock is integrated from an empirical fit, not solved from the Euler equations.
- No Richtmyer-Meshkov or Kelvin-Helmholtz instabilities are resolved; turbulence is
  represented by procedural noise and particles.
- The fireball/cloud geometry is axisymmetric and analytic; wind shear and terrain
  interaction are not modelled.
- Positive-phase duration is a first-order estimate.
- Thermal and blast radii assume an unobstructed free-air burst.

For a first-principles treatment the natural next step is an axisymmetric compressible
Euler solver with an HLLC Riemann solver and soot advection, used to fill the same volume
texture. Such a solver is significantly more expensive and is not interactive at the
resolution required for a convincing fireball, which is why the reduced-order model is
the default.

## References

1. Brode, H. L. (1955). *Numerical Solutions of Spherical Blast Waves.* Journal of
   Applied Physics 26(6).
2. Kinney, G. F., Graham, K. J. (1985). *Explosive Shocks in Air.* Springer.
3. Sedov, L. I. (1959). *Similarity and Dimensional Methods in Mechanics.*
4. Glasstone, S., Dolan, P. J. (1977). *The Effects of Nuclear Weapons.* US DoD/DoE.
5. Kim, Y., Cho, Y., Kim, Y. (2002). *A colour temperature conversion algorithm.*
6. Narkowicz, K. (2015). *ACES filmic tone mapping.*
