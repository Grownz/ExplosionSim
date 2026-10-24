#ifndef EXPLOSION_EULER_COMMON_HLSLI
#define EXPLOSION_EULER_COMMON_HLSLI

// Shared definitions for the GPU compressible Euler solver.
//
// Conserved variables are split over two RGBA32F volumes:
//   StateA = (rho, rho*u, rho*v, rho*w)   (u = x, v = y/up, w = z)
//   StateB = (E, rho*Y, 0, 1)             (Y = detonation-product mass fraction)
//
// The solver uses a finite-volume update with HLLC fluxes and MUSCL/minmod
// reconstruction, dimensional splitting and a buoyancy/radiation source pass.

static const float PI = 3.14159265359;

cbuffer SolverConstants : register(b0)
{
    int4 Grid;          // xyz = resolution, w = sweep direction
    float4 DomainMin;   // xyz = lower corner, w = cell size dx
    float4 DomainMax;   // xyz = upper corner, w = time step dt
    float4 Ambient;     // rho0, p0, T0, gamma
    float4 Charge;      // mass, energy, radius, height of burst
    float4 Physics;     // gravity, gas constant, cooling, simulation time
    float4 Extra;       // reserved
};

Texture3D<float4> StateA : register(t0);
Texture3D<float4> StateB : register(t1);

RWTexture3D<float4> OutA : register(u0);
RWTexture3D<float4> OutB : register(u1);

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

// Ground (y = 0) is a reflecting wall: mirror the ghost cell and reverse the
// vertical momentum. x/z and the top boundary are zero-gradient outflows.
float4 LoadA(int3 p)
{
    const bool reflected = p.y < 0;
    if (reflected)
    {
        p.y = -p.y - 1;
    }
    float4 state = StateA.Load(int4(ClampCell(p), 0));
    if (reflected)
    {
        state.z = -state.z;
    }
    return state;
}

float4 LoadB(int3 p)
{
    if (p.y < 0)
    {
        p.y = -p.y - 1;
    }
    return StateB.Load(int4(ClampCell(p), 0));
}

Primitive LoadPrimitive(int3 p)
{
    float4 a = LoadA(p);
    float4 b = LoadB(p);

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

float Minmod(float a, float b)
{
    return (a * b <= 0.0) ? 0.0 : (abs(a) < abs(b) ? a : b);
}

// MUSCL reconstruction with minmod limiting. Falls back to the cell-centred
// state if the reconstructed density or pressure becomes non-physical.
Primitive Reconstruct(Primitive left, Primitive center, Primitive right, float sign,
                      Primitive fallback)
{
    Primitive r;
    r.rho = center.rho + sign * 0.5 * Minmod(center.rho - left.rho, right.rho - center.rho);
    r.u = center.u + sign * 0.5 * Minmod(center.u - left.u, right.u - center.u);
    r.v = center.v + sign * 0.5 * Minmod(center.v - left.v, right.v - center.v);
    r.w = center.w + sign * 0.5 * Minmod(center.w - left.w, right.w - center.w);
    r.p = center.p + sign * 0.5 * Minmod(center.p - left.p, right.p - center.p);
    r.y = saturate(center.y + sign * 0.5 * Minmod(center.y - left.y, right.y - center.y));

    bool invalid = r.rho <= 1e-6 || r.p <= 1e-3 || !isfinite(r.rho) || !isfinite(r.p);
    if (invalid)
    {
        return fallback;
    }
    return r;
}

float NormalVelocity(Primitive s, int dir)
{
    return dir == 0 ? s.u : (dir == 1 ? s.v : s.w);
}

// HLLC approximate Riemann solver. Returns the fluxes for the (rho,momentum)
// and (energy, soot) groups.
void HllcFlux(Primitive L, Primitive R, int dir, out float4 fluxA, out float2 fluxB)
{
    const float gamma = Ambient.w;

    float unL = NormalVelocity(L, dir);
    float unR = NormalVelocity(R, dir);

    float cL = sqrt(gamma * L.p / L.rho);
    float cR = sqrt(gamma * R.p / R.rho);

    float SL = min(unL - cL, unR - cR);
    float SR = max(unL + cL, unR + cR);

    float EL = L.p / (gamma - 1.0) + 0.5 * L.rho * (L.u * L.u + L.v * L.v + L.w * L.w);
    float ER = R.p / (gamma - 1.0) + 0.5 * R.rho * (R.u * R.u + R.v * R.v + R.w * R.w);

    float4 ULA = float4(L.rho, L.rho * L.u, L.rho * L.v, L.rho * L.w);
    float4 URA = float4(R.rho, R.rho * R.u, R.rho * R.v, R.rho * R.w);
    float2 ULB = float2(EL, L.rho * L.y);
    float2 URB = float2(ER, R.rho * R.y);

    float4 FLA = float4(L.rho * unL, L.rho * L.u * unL, L.rho * L.v * unL, L.rho * L.w * unL);
    FLA.y += (dir == 0) ? L.p : 0.0;
    FLA.z += (dir == 1) ? L.p : 0.0;
    FLA.w += (dir == 2) ? L.p : 0.0;
    float2 FLB = float2((EL + L.p) * unL, L.rho * L.y * unL);

    float4 FRA = float4(R.rho * unR, R.rho * R.u * unR, R.rho * R.v * unR, R.rho * R.w * unR);
    FRA.y += (dir == 0) ? R.p : 0.0;
    FRA.z += (dir == 1) ? R.p : 0.0;
    FRA.w += (dir == 2) ? R.p : 0.0;
    float2 FRB = float2((ER + R.p) * unR, R.rho * R.y * unR);

    float denom = L.rho * (SL - unL) - R.rho * (SR - unR);
    if (abs(denom) < 1e-8)
    {
        // Degenerate star region: fall back to a Rusanov flux.
        float amax = max(abs(unL) + cL, abs(unR) + cR);
        fluxA = 0.5 * (FLA + FRA) - 0.5 * amax * (URA - ULA);
        fluxB = 0.5 * (FLB + FRB) - 0.5 * amax * (URB - ULB);
        return;
    }

    float SM = (R.p - L.p + L.rho * unL * (SL - unL) - R.rho * unR * (SR - unR)) / denom;

    if (SL >= 0.0)
    {
        fluxA = FLA;
        fluxB = FLB;
        return;
    }

    if (SM >= 0.0)
    {
        float starDenom = SL - SM;
        if (abs(starDenom) < 1e-8)
        {
            fluxA = FLA;
            fluxB = FLB;
            return;
        }

        float rhoStar = L.rho * (SL - unL) / starDenom;
        float eStar = EL / L.rho + (SM - unL) * (SM + L.p / (L.rho * (SL - unL)));
        float4 UAstar = float4(rhoStar,
                               rhoStar * (dir == 0 ? SM : L.u),
                               rhoStar * (dir == 1 ? SM : L.v),
                               rhoStar * (dir == 2 ? SM : L.w));
        float2 UBstar = float2(rhoStar * eStar, rhoStar * L.y);
        fluxA = FLA + SL * (UAstar - ULA);
        fluxB = FLB + SL * (UBstar - ULB);
        return;
    }

    if (SR > 0.0)
    {
        float starDenom = SR - SM;
        if (abs(starDenom) < 1e-8)
        {
            fluxA = FRA;
            fluxB = FRB;
            return;
        }

        float rhoStar = R.rho * (SR - unR) / starDenom;
        float eStar = ER / R.rho + (SM - unR) * (SM + R.p / (R.rho * (SR - unR)));
        float4 UAstar = float4(rhoStar,
                               rhoStar * (dir == 0 ? SM : R.u),
                               rhoStar * (dir == 1 ? SM : R.v),
                               rhoStar * (dir == 2 ? SM : R.w));
        float2 UBstar = float2(rhoStar * eStar, rhoStar * R.y);
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
    if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(a.z) || !isfinite(a.w) ||
        !isfinite(b.x) || !isfinite(b.y))
    {
        a = float4(Ambient.x, 0.0, 0.0, 0.0);
        b = float2(Ambient.y / (Ambient.w - 1.0), 0.0);
        return;
    }

    a.x = max(a.x, 1e-6);
    float3 velocity = a.yzw / a.x;
    float kinetic = 0.5 * a.x * dot(velocity, velocity);
    b.x = max(b.x, kinetic + 1e-3);
    b.y = max(b.y, 0.0);
}

float AmbientTemperature(float rho, float p)
{
    return p / max(rho * Physics.y, 1e-3);
}

#endif // EXPLOSION_EULER_COMMON_HLSLI
