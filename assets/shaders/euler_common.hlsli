#ifndef EXPLOSION_EULER_COMMON_HLSLI
#define EXPLOSION_EULER_COMMON_HLSLI

// Shared definitions for the GPU compressible Euler solver.
//
// Conserved variables are split over two volumes:
//   StateA = (rho, rho*u, rho*v, rho*w)   RGBA32F (u = x, v = y/up, w = z)
//   StateB = (E, rho*Y)                   RG32F   (Y = detonation-product mass)
//
// The solver uses a finite-volume update with HLLC fluxes and MUSCL/minmod
// reconstruction and dimensional splitting. The buoyancy/radiation source terms
// are fused into the Z sweep, so a step is three dispatches.

static const float PI = 3.14159265359;

// Vulkan uses explicit descriptor bindings. D3D11 register spaces do not map
// one-to-one onto Vulkan descriptor sets, so each solver resource gets a fixed
// binding: b0 -> 0, t0/t1 -> 8/9, s0 -> 16, u0/u1 -> 21/22.
#if defined(EXPLOSION_VULKAN)
#define EX_VK_BINDING(n) [[vk::binding(n, 0)]]
#else
#define EX_VK_BINDING(n)
#endif

// Thread-group size, overridable from the C++ side so the occupancy can be
// tuned without editing every shader. The reduce pass derives its linear
// thread index from these values.
#ifndef EULER_GROUP_X
#define EULER_GROUP_X 8
#endif
#ifndef EULER_GROUP_Y
#define EULER_GROUP_Y 8
#endif
#ifndef EULER_GROUP_Z
#define EULER_GROUP_Z 8
#endif

EX_VK_BINDING(0) cbuffer SolverConstants : register(b0)
{
    int4 Grid;          // xyz = resolution, w = sweep direction
    float4 DomainMin;   // xyz = lower corner, w = cell size dx
    float4 DomainMax;   // xyz = upper corner, w = time step dt
    float4 Ambient;     // rho0, p0, T0, gamma
    float4 Charge;      // mass, energy, radius, height of burst
    float4 Physics;     // gravity, gas constant, cooling, simulation time
    float4 Extra;       // x = plume radius for the entrainment drag, z = hemisphere flag
    float4 OldDomainMin; // xyz = old lower corner, w = old dx (for rescaling)
    float4 OldDomainMax; // xyz = old upper corner
    float4 SunDirection; // xyz = normalized direction to the sun
    float4 LightParams;  // x = extinction scale, y = light raymarch steps
    float4 CapParams;    // x = cap radius, y = cap centre, z = cap top, w = cap height threshold
    float4 EnvParams;    // x = stable layer height (0 = off), y = initial top, z = analytic rise speed
};

EX_VK_BINDING(16) SamplerState LinearSampler : register(s0);

EX_VK_BINDING(8) Texture3D<float4> StateA : register(t0);
EX_VK_BINDING(9) Texture3D<float2> StateB : register(t1);
// Solid mask from the scene (a = 1 solid, 0 fluid; rgb = wall velocity). Its
// resolution is independent of the solver grid; cell indices are scaled in the
// shaders.
EX_VK_BINDING(11) Texture3D<float4> SolidMask : register(t3);

EX_VK_BINDING(21) RWTexture3D<float4> OutA : register(u0);
EX_VK_BINDING(22) RWTexture3D<float2> OutB : register(u1);

struct Primitive
{
    float rho;
    float u;
    float v;
    float w;
    float p;
    float y;
};

int3 ClampCell(int3 p)
{
    return clamp(p, int3(0, 0, 0), Grid.xyz - 1);
}

// --- Domain shape -------------------------------------------------------
// Cube (default): the whole Cartesian box is active.
// Hemisphere: the box is masked to a semi-ellipsoid with the lateral radius
// 0.5 * (max.x - min.x) and the vertical semi-axis max.y. This keeps the
// uniform Cartesian grid and its CFL behaviour, but treats the corners as
// ambient/absorbing. The mask is normalized, so it scales with auto-scale.
bool IsHemispherical()
{
    return Extra.z > 0.5;
}

float DomainLateralHalf()
{
    return max(0.5 * (DomainMax.x - DomainMin.x), 1e-3);
}

float DomainFractionSquared(float3 world)
{
    const float halfX = DomainLateralHalf();
    const float top = max(DomainMax.y, 1e-3);
    const float centerX = 0.5 * (DomainMin.x + DomainMax.x);
    const float centerZ = 0.5 * (DomainMin.z + DomainMax.z);
    const float x = (world.x - centerX) / halfX;
    const float z = (world.z - centerZ) / halfX;
    const float y = world.y / top;
    return x * x + y * y + z * z;
}

float DomainFraction(float3 world)
{
    return sqrt(DomainFractionSquared(world));
}

bool IsActiveWorld(float3 world)
{
    if (!IsHemispherical())
    {
        return true;
    }
    return world.y >= 0.0 && DomainFractionSquared(world) <= 1.0;
}

bool IsActiveCell(int3 p)
{
    if (!IsHemispherical())
    {
        return true;
    }
    const float3 world = DomainMin.xyz + (float3(p) + 0.5) * DomainMin.w;
    return IsActiveWorld(world);
}

// --- Solid obstacles ----------------------------------------------------
// The scene is voxelized into a mask that may use a lower resolution than the
// solver grid, so a solver cell maps to the nearest mask cell.
int3 SolidMaskCell(int3 p)
{
    uint3 size;
    SolidMask.GetDimensions(size.x, size.y, size.z);
    const float3 scale = float3(size) / float3(Grid.xyz);
    return clamp(int3((float3(p) + 0.5) * scale), int3(0, 0, 0),
                 int3(size) - 1);
}

bool IsSolidCell(int3 p)
{
    if (any(p < int3(0, 0, 0)) || any(p >= Grid.xyz))
    {
        return false;
    }
    return SolidMask.Load(int4(SolidMaskCell(p), 0)).a > 0.5;
}

// Velocity of the solid wall at a cell (zero for static scene boxes). The
// no-slip ghost mirrors around the moving wall so debris drags the flow.
float3 SolidWallVelocity(int3 p)
{
    if (any(p < int3(0, 0, 0)) || any(p >= Grid.xyz))
    {
        return float3(0.0, 0.0, 0.0);
    }
    return SolidMask.Load(int4(SolidMaskCell(p), 0)).rgb;
}

// No-slip ghost state. The face velocity must equal the wall velocity, so the
// ghost velocity mirrors the fluid velocity around it: u_ghost = 2 u_wall - u.
Primitive MirrorWall(Primitive center, float3 wallVelocity)
{
    Primitive ghost = center;
    ghost.u = 2.0 * wallVelocity.x - center.u;
    ghost.v = 2.0 * wallVelocity.y - center.v;
    ghost.w = 2.0 * wallVelocity.z - center.w;
    return ghost;
}

// Ground (y = 0) is a reflecting wall: mirror the ghost cell and reverse the
// vertical momentum. x/z and the top boundary are zero-gradient outflows.
float4 LoadA(int3 p)
{
    const bool reflected = p.y < 0;
    if (reflected)
    {
        p.y = -p.y - 1;
    }
    const int3 cell = ClampCell(p);
    if (!IsActiveCell(cell))
    {
        return float4(Ambient.x, 0.0, 0.0, 0.0);
    }
    float4 state = StateA.Load(int4(cell, 0));
    if (reflected)
    {
        state.z = -state.z;
    }
    return state;
}

float2 LoadB(int3 p)
{
    if (p.y < 0)
    {
        p.y = -p.y - 1;
    }
    const int3 cell = ClampCell(p);
    if (!IsActiveCell(cell))
    {
        return float2(Ambient.y / (Ambient.w - 1.0), 0.0);
    }
    return StateB.Load(int4(cell, 0));
}

Primitive MakePrimitive(float4 a, float2 b)
{
    Primitive s;
    s.rho = max(a.x, 1e-6);
    s.u = a.y / s.rho;
    s.v = a.z / s.rho;
    s.w = a.w / s.rho;

    float kinetic = 0.5 * s.rho * (s.u * s.u + s.v * s.v + s.w * s.w);
    s.p = max((Ambient.w - 1.0) * (b.x - kinetic), 1e-3);
    s.y = saturate(b.y / s.rho);
    return s;
}

// Conserved state plus its primitive form. The sweep needs both for the centre
// cell, so they are loaded and decoded together.
struct CellState
{
    float4 a;
    float2 b;
    Primitive prim;
};

CellState LoadCell(int3 p)
{
    CellState c;
    c.a = LoadA(p);
    c.b = LoadB(p);
    c.prim = MakePrimitive(c.a, c.b);
    return c;
}

Primitive LoadPrimitive(int3 p)
{
    CellState c = LoadCell(p);
    return c.prim;
}

float Minmod(float a, float b)
{
    return (a * b <= 0.0) ? 0.0 : (abs(a) < abs(b) ? a : b);
}

// MUSCL slope (0.5 * minmod) of a three-cell stencil. The middle stencil is
// shared by the left and right face reconstructions of the same cell.
Primitive Slope(Primitive left, Primitive center, Primitive right)
{
    Primitive s;
    s.rho = 0.5 * Minmod(center.rho - left.rho, right.rho - center.rho);
    s.u = 0.5 * Minmod(center.u - left.u, right.u - center.u);
    s.v = 0.5 * Minmod(center.v - left.v, right.v - center.v);
    s.w = 0.5 * Minmod(center.w - left.w, right.w - center.w);
    s.p = 0.5 * Minmod(center.p - left.p, right.p - center.p);
    s.y = 0.5 * Minmod(center.y - left.y, right.y - center.y);
    return s;
}

// Applies a precomputed slope with the given sign. Falls back to the
// cell-centred state if the reconstructed density or pressure becomes
// non-physical.
Primitive ApplySlope(Primitive center, Primitive slope, float sign, Primitive fallback)
{
    Primitive r;
    r.rho = center.rho + sign * slope.rho;
    r.u = center.u + sign * slope.u;
    r.v = center.v + sign * slope.v;
    r.w = center.w + sign * slope.w;
    r.p = center.p + sign * slope.p;
    r.y = saturate(center.y + sign * slope.y);

    if (!all(isfinite(float2(r.rho, r.p))) || r.rho <= 1e-6 || r.p <= 1e-3)
    {
        return fallback;
    }
    return r;
}

// MUSCL reconstruction with minmod limiting.
Primitive Reconstruct(Primitive left, Primitive center, Primitive right, float sign,
                      Primitive fallback)
{
    return ApplySlope(center, Slope(left, center, right), sign, fallback);
}

float NormalVelocity(Primitive s, int dir)
{
    return dir == 0 ? s.u : (dir == 1 ? s.v : s.w);
}

// HLLC approximate Riemann solver. Returns the fluxes for the (rho,momentum)
// and (energy, soot) groups. The direction selects fold away in the
// direction-specific sweep entry points, and the repeated products/energies are
// hoisted so each is evaluated once per face.
void HllcFlux(Primitive L, Primitive R, int dir, out float4 fluxA, out float2 fluxB)
{
    const float gamma = Ambient.w;

    const float unL = NormalVelocity(L, dir);
    const float unR = NormalVelocity(R, dir);

    const float cL = sqrt(gamma * L.p / L.rho);
    const float cR = sqrt(gamma * R.p / R.rho);

    const float SL = min(unL - cL, unR - cR);
    const float SR = max(unL + cL, unR + cR);

    const float vvL = L.u * L.u + L.v * L.v + L.w * L.w;
    const float vvR = R.u * R.u + R.v * R.v + R.w * R.w;
    const float EL = L.p / (gamma - 1.0) + 0.5 * L.rho * vvL;
    const float ER = R.p / (gamma - 1.0) + 0.5 * R.rho * vvR;

    const float mxL = L.rho * L.u;
    const float myL = L.rho * L.v;
    const float mzL = L.rho * L.w;
    const float mxR = R.rho * R.u;
    const float myR = R.rho * R.v;
    const float mzR = R.rho * R.w;

    const float4 ULA = float4(L.rho, mxL, myL, mzL);
    const float4 URA = float4(R.rho, mxR, myR, mzR);
    const float2 ULB = float2(EL, L.rho * L.y);
    const float2 URB = float2(ER, R.rho * R.y);

    float4 FLA = float4(L.rho * unL, mxL * unL, myL * unL, mzL * unL);
    FLA.y += (dir == 0) ? L.p : 0.0;
    FLA.z += (dir == 1) ? L.p : 0.0;
    FLA.w += (dir == 2) ? L.p : 0.0;
    const float2 FLB = float2((EL + L.p) * unL, L.rho * L.y * unL);

    float4 FRA = float4(R.rho * unR, mxR * unR, myR * unR, mzR * unR);
    FRA.y += (dir == 0) ? R.p : 0.0;
    FRA.z += (dir == 1) ? R.p : 0.0;
    FRA.w += (dir == 2) ? R.p : 0.0;
    const float2 FRB = float2((ER + R.p) * unR, R.rho * R.y * unR);

    const float SLm = SL - unL;
    const float SRm = SR - unR;
    const float denom = L.rho * SLm - R.rho * SRm;
    if (abs(denom) < 1e-8)
    {
        // Degenerate star region: fall back to a Rusanov flux.
        const float amax = max(abs(unL) + cL, abs(unR) + cR);
        fluxA = 0.5 * (FLA + FRA) - 0.5 * amax * (URA - ULA);
        fluxB = 0.5 * (FLB + FRB) - 0.5 * amax * (URB - ULB);
        return;
    }

    const float SM = (R.p - L.p + L.rho * unL * SLm - R.rho * unR * SRm) / denom;

    if (SL >= 0.0)
    {
        fluxA = FLA;
        fluxB = FLB;
        return;
    }

    if (SM >= 0.0)
    {
        const float starDenom = SL - SM;
        if (abs(starDenom) < 1e-8)
        {
            fluxA = FLA;
            fluxB = FLB;
            return;
        }

        const float rhoStar = L.rho * SLm / starDenom;
        const float eStar = EL / L.rho + (SM - unL) * (SM + L.p / (L.rho * SLm));
        const float4 UAstar = float4(rhoStar,
                                     rhoStar * (dir == 0 ? SM : L.u),
                                     rhoStar * (dir == 1 ? SM : L.v),
                                     rhoStar * (dir == 2 ? SM : L.w));
        const float2 UBstar = float2(rhoStar * eStar, rhoStar * L.y);
        fluxA = FLA + SL * (UAstar - ULA);
        fluxB = FLB + SL * (UBstar - ULB);
        return;
    }

    if (SR > 0.0)
    {
        const float starDenom = SR - SM;
        if (abs(starDenom) < 1e-8)
        {
            fluxA = FRA;
            fluxB = FRB;
            return;
        }

        const float rhoStar = R.rho * SRm / starDenom;
        const float eStar = ER / R.rho + (SM - unR) * (SM + R.p / (R.rho * SRm));
        const float4 UAstar = float4(rhoStar,
                                     rhoStar * (dir == 0 ? SM : R.u),
                                     rhoStar * (dir == 1 ? SM : R.v),
                                     rhoStar * (dir == 2 ? SM : R.w));
        const float2 UBstar = float2(rhoStar * eStar, rhoStar * R.y);
        fluxA = FRA + SR * (UAstar - URA);
        fluxB = FRB + SR * (UBstar - URB);
        return;
    }

    fluxA = FRA;
    fluxB = FRB;
}

// Restores positivity and protects against non-finite values.
void Sanitise(inout float4 a, inout float2 b)
{
    if (!all(isfinite(a)) || !all(isfinite(b)))
    {
        a = float4(Ambient.x, 0.0, 0.0, 0.0);
        b = float2(Ambient.y / (Ambient.w - 1.0), 0.0);
        return;
    }

    a.x = max(a.x, 1e-6);
    const float3 velocity = a.yzw / a.x;
    const float kinetic = 0.5 * a.x * dot(velocity, velocity);
    b.x = max(b.x, kinetic + 1e-3);
    b.y = max(b.y, 0.0);
}

float AmbientTemperature(float rho, float p)
{
    return p / max(rho * Physics.y, 1e-3);
}

// --- Source terms -------------------------------------------------------
// Buoyancy, sub-grid entrainment drag, cap closure, radiative cooling and the
// open-boundary sponge. Fused into the Z sweep: the caller passes the state it
// has just updated, which is exactly what the old separate source pass loaded
// back from the Z output.
static const float kSpongeWidth = 0.06;
static const float kSpongeRate = 45.0;

void ApplySource(inout float4 a, inout float2 b, int3 cell)
{
    const float rho = max(a.x, 1e-6);
    const float3 velocity = a.yzw / rho;
    float energy = b.x;
    float soot = b.y;

    const float pressure =
        max((Ambient.w - 1.0) * (energy - 0.5 * rho * dot(velocity, velocity)), 1e-3);
    const float temperature = AmbientTemperature(rho, pressure);
    const float3 world = DomainMin.xyz + (float3(cell) + 0.5) * DomainMin.w;

    // Net buoyancy (only the density difference relative to the ambient air).
    float excess = rho - Ambient.x;
    a.z -= excess * Physics.x * DomainMax.w;
    energy -= excess * Physics.x * velocity.y * DomainMax.w;

    // Sub-grid entrainment drag. The unresolved mixing slows the buoyant plume;
    // without it the coarse grid lets the hot products rise as a narrow jet.
    // The analytic cloud model uses the same closure (0.6 * v * |v| / 2R).
    float plumeRadius = max(Extra.x, 2.0 * DomainMin.w);
    float thermal = saturate((temperature - Ambient.z) / 300.0);
    float light = saturate((Ambient.x - rho) / max(Ambient.x, 1e-3));
    float sootMask = saturate(soot / 0.0005);
    float plumeMask = saturate(max(max(thermal, light), sootMask));
    float speed = length(velocity);
    float drag = 0.6 * speed / (2.0 * plumeRadius);
    a.yzw -= rho * velocity * drag * plumeMask * DomainMax.w;
    energy -= rho * dot(velocity, velocity) * drag * plumeMask * DomainMax.w;

    // One-way coupling to the analytic thermal. The coarse grid diffuses the
    // fireball heat, so the resolved plume rises slower than the analytic cloud
    // that drives the camera, dust and particles. A one-sided relaxation in the
    // upper plume adds the missing upward momentum and keeps both on one
    // trajectory without flattening the resolved velocity structure.
    const float riseTarget = EnvParams.z;
    if (riseTarget > 0.0)
    {
        // Only the head is guided: lifting the whole plume would raise the stem
        // and base with it and turn the cloud into a column. The base stays
        // buoyancy-driven; the head follows the analytic rise and stretches the
        // stem behind it.
        const float guideHeight =
            smoothstep(0.45 * CapParams.z, 0.75 * CapParams.z, world.y);
        const float guide = 4.0 * plumeMask * guideHeight;
        // One-sided: only add the missing upward momentum. Early on the blast
        // drives the fireball faster than the analytic thermal, and slowing it
        // down would drag the head below the analytic cloud.
        const float guideImpulse =
            rho * max(riseTarget - velocity.y, 0.0) * guide * DomainMax.w;
        a.z += guideImpulse;
        energy += guideImpulse * velocity.y;
    }

    // Sub-grid cap closure. The mushroom head is a toroidal vortex: the ring
    // core sits at the vertical centre of the head, the flow curls outward over
    // the top and back down the outside, and the inner branch feeds the stem.
    //
    // Geometry follows Glasstone & Dolan (The Effects of Nuclear Weapons,
    // 2.17): for a cloud that has not reached the tropopause the base of the
    // head is roughly at half the altitude of the top. A developed head
    // therefore spans [0.5 T, T] with its core at 0.75 T. A young, still
    // spherical head keeps the thermal scaling C = T - R; capping the offset at
    // T/4 blends between the two without a special case.
    float3 radial = float3(world.x, 0.0, world.z);
    float radialLength = length(radial);
    float3 radialDirection = radial / max(radialLength, 1e-3);
    // The head geometry comes from the renderer: the analytic cap centre and
    // top, so the closure travels with the reference cloud instead of braking
    // the resolved plume at its own (lagging) measured top.
    const float capTop = max(CapParams.z, 2.0 * DomainMin.w);
    const float targetRadius = max(CapParams.x, 2.0 * DomainMin.w);
    const float targetCenterY = CapParams.y;
    const float upperHalf =
        max(capTop - targetCenterY, 2.0 * DomainMin.w);
    float dr = radialLength - targetRadius;
    float dy = world.y - targetCenterY;
    // The vortex tube is slightly oblate: wide across the head, a little
    // thinner vertically, which keeps the cap from spinning into a ball.
    const float ringAspect = 1.25;
    const float aspectSq = ringAspect * ringAspect;
    float ringDistance = sqrt(dr * dr + dy * dy * aspectSq);
    float ringWidth = max(0.45 * targetRadius, 2.0 * DomainMin.w);
    float ring = exp(-pow(ringDistance / ringWidth, 2.0));
    float2 tangent = float2(dy * aspectSq, -dr) /
                     max(sqrt(dr * dr + dy * dy * aspectSq * aspectSq), 1e-3);
    float strength = 30.0 * ring * plumeMask;
    float impulse = rho * strength * DomainMax.w;
    a.y += radialDirection.x * tangent.x * impulse;
    a.z += tangent.y * impulse;
    a.w += radialDirection.z * tangent.x * impulse;
    energy += impulse * (tangent.x * dot(radialDirection, velocity) +
                         tangent.y * velocity.y);

    float above = saturate((world.y - capTop) / max(upperHalf, 2.0 * DomainMin.w));
    float verticalDamping = 4.0 * above;
    a.z -= rho * max(velocity.y, 0.0) * verticalDamping * DomainMax.w;
    energy -= 0.5 * rho * max(velocity.y, 0.0) * max(velocity.y, 0.0) *
              verticalDamping * DomainMax.w;

    // Flatten the cap top. The toroidal ring is weak at the centre because the
    // ring distance is roughly one cap radius there, so the central column can
    // keep rising and round the head off. This band damps the upward motion at
    // the top near the axis.
    const float topBand =
        exp(-pow((world.y - capTop) / max(0.5 * upperHalf, 2.0 * DomainMin.w), 2.0));
    const float axisBand =
        exp(-pow(radialLength / max(0.90 * targetRadius, 2.0 * DomainMin.w), 2.0));
    const float flatten = 8.0 * topBand * axisBand * plumeMask;
    a.z -= rho * max(velocity.y, 0.0) * flatten * DomainMax.w;
    energy -= 0.5 * rho * max(velocity.y, 0.0) * max(velocity.y, 0.0) * flatten *
              DomainMax.w;

    // Stable-layer surrogate. The analytic reference cloud has no stratification
    // and stops through entrainment drag alone, so the lid is disabled here
    // (stableLayerY_ = 0). A stratified environment can set it again.
    const float stableBand = EnvParams.x > 0.0
                                 ? saturate((world.y - EnvParams.x) /
                                            max(0.10 * EnvParams.y, 2.0 * DomainMin.w))
                                 : 0.0;
    const float stableDamping = 3.0 * stableBand * plumeMask;
    a.z -= rho * max(velocity.y, 0.0) * stableDamping * DomainMax.w;
    energy -= 0.5 * rho * max(velocity.y, 0.0) * max(velocity.y, 0.0) * stableDamping *
              DomainMax.w;

    // Radiative cooling of hot soot.
    energy -= Physics.z * soot * max(temperature - Ambient.z, 0.0) * DomainMax.w;

    b.x = energy;
    b.y = soot;

    // --- Open-boundary sponge --------------------------------------------
    float blend = 0.0;
    if (IsHemispherical())
    {
        const float q = DomainFraction(world);
        const float ramp = saturate((q - (1.0 - kSpongeWidth)) / kSpongeWidth);
        blend = saturate(kSpongeRate * ramp * ramp * DomainMax.w);
    }
    else
    {
        float3 extent = max(DomainMax.xyz - DomainMin.xyz, 1e-4);
        float3 rel = (world - DomainMin.xyz) / extent;
        float lateral = min(min(rel.x, 1.0 - rel.x), min(rel.z, 1.0 - rel.z));
        float toTop = 1.0 - rel.y;
        float edge = min(lateral, toTop);
        float ramp = saturate(1.0 - edge / kSpongeWidth);
        blend = saturate(kSpongeRate * ramp * ramp * DomainMax.w);
    }

    if (blend > 1e-5)
    {
        float ambientEnergy = Ambient.y / (Ambient.w - 1.0);
        a = lerp(a, float4(Ambient.x, 0.0, 0.0, 0.0), blend);
        b = lerp(b, float2(ambientEnergy, 0.0), blend);
    }

    Sanitise(a, b);
}

#endif // EXPLOSION_EULER_COMMON_HLSLI
