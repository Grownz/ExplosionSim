#include "euler_common.hlsli"

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
    Sanitise(a, b);

    OutA[id] = a;
    OutB[id] = float4(b, 0.0, 1.0);
}
