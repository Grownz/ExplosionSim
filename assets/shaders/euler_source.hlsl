#include "euler_common.hlsli"

// Absorbing sponge at the lateral and top boundaries. The ground stays a
// reflecting wall. A zero-gradient ghost cell alone reflects acoustic waves;
// relaxing the state towards ambient over the outer few percent of the domain
// lets outgoing waves leave without a visible echo.
static const float kSpongeWidth = 0.06;
static const float kSpongeRate = 45.0;

// Buoyancy in a uniform ambient atmosphere plus a sub-grid radiative cooling
// term for the soot. The hydrostatic background is subtracted so the ambient
// air does not accelerate downwards.
[numthreads(8, 8, 8)]
void CSSource(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    float4 a = StateA.Load(int4(id, 0));
    float2 b = StateB.Load(int4(id, 0)).xy;

    float rho = max(a.x, 1e-6);
    float3 velocity = a.yzw / rho;
    float energy = b.x;
    float soot = b.y;

    float pressure = max((Ambient.w - 1.0) * (energy - 0.5 * rho * dot(velocity, velocity)), 1e-3);
    float temperature = AmbientTemperature(rho, pressure);

    // Net buoyancy (only the density difference relative to the ambient air).
    float excess = rho - Ambient.x;
    a.z -= excess * Physics.x * DomainMax.w;
    energy -= excess * Physics.x * velocity.y * DomainMax.w;

    // Radiative cooling of hot soot.
    energy -= Physics.z * soot * max(temperature - Ambient.z, 0.0) * DomainMax.w;

    b.x = energy;
    b.y = soot;

    // --- Open-boundary sponge --------------------------------------------
    float3 world = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
    float3 extent = max(DomainMax.xyz - DomainMin.xyz, 1e-4);
    float3 rel = (world - DomainMin.xyz) / extent;
    float lateral = min(min(rel.x, 1.0 - rel.x), min(rel.z, 1.0 - rel.z));
    float toTop = 1.0 - rel.y;
    float edge = min(lateral, toTop);
    float ramp = saturate(1.0 - edge / kSpongeWidth);
    float blend = saturate(kSpongeRate * ramp * ramp * DomainMax.w);

    if (blend > 1e-5)
    {
        float ambientEnergy = Ambient.y / (Ambient.w - 1.0);
        a = lerp(a, float4(Ambient.x, 0.0, 0.0, 0.0), blend);
        b = lerp(b, float2(ambientEnergy, 0.0), blend);
    }

    Sanitise(a, b);

    OutA[id] = a;
    OutB[id] = float4(b, 0.0, 1.0);
}
