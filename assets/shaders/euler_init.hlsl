#include "euler_common.hlsli"

// Fills the domain with ambient air and deposits the TNT charge as a sphere of
// detonation products. The initial energy density sets the pressure through the
// ideal-gas equation of state.
[numthreads(8, 8, 8)]
void CSInit(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    float3 position = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
    float3 offset = position - float3(0.0, Charge.w, 0.0);

    float rho = Ambient.x;
    float energy = Ambient.y / (Ambient.w - 1.0);
    float sootDensity = 0.0;

    if (length(offset) < Charge.z)
    {
        float volume = (4.0 / 3.0) * PI * Charge.z * Charge.z * Charge.z;
        rho = Charge.x / volume;
        energy = Charge.y / volume;
        sootDensity = rho;
    }

    OutA[id] = float4(rho, 0.0, 0.0, 0.0);
    OutB[id] = float4(energy, sootDensity, 0.0, 1.0);
}
