#include "euler_common.hlsli"

// Fills the domain with ambient air and deposits the TNT charge as a sphere of
// detonation products. The initial energy density sets the pressure through the
// ideal-gas equation of state.
//
// For low bursts part of the sphere lies below the ground and is not present in
// the grid. The density and energy density are therefore scaled by the ratio of
// the full sphere volume to the above-ground volume, so the total mass and
// energy of the charge are preserved instead of being silently truncated.
[numthreads(EULER_GROUP_X, EULER_GROUP_Y, EULER_GROUP_Z)]
void CSInit(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    if (IsSolidCell(int3(id)))
    {
        OutA[id] = float4(Ambient.x, 0.0, 0.0, 0.0);
        OutB[id] = float2(Ambient.y / (Ambient.w - 1.0), 0.0);
        return;
    }

    float3 position = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
    float3 offset = position - float3(0.0, Charge.w, 0.0);

    float rho = Ambient.x;
    float energy = Ambient.y / (Ambient.w - 1.0);
    float sootDensity = 0.0;

    const float radius = max(Charge.z, 1e-4);
    const float fullVolume = (4.0 / 3.0) * PI * radius * radius * radius;
    const float capHeight = clamp(radius - Charge.w, 0.0, 2.0 * radius);
    const float belowVolume = PI * capHeight * capHeight *
                              (3.0 * radius - capHeight) / 3.0;
    const float aboveVolume = max(fullVolume - belowVolume, 1e-6);

    const float distance = length(offset);
    // Anti-aliased sphere: distribute the charge over the boundary cells so the
    // initial condition does not seed the Cartesian grid with stair steps.
    const float fraction =
        saturate((radius - distance) / max(DomainMin.w, 1e-4) + 0.5);
    if (fraction > 0.0)
    {
        const float chargeDensity = Charge.x / aboveVolume;
        const float chargeEnergy = Charge.y / aboveVolume;
        rho = lerp(rho, chargeDensity, fraction);
        energy = lerp(energy, chargeEnergy, fraction);
        sootDensity = chargeDensity * fraction;
    }

    OutA[id] = float4(rho, 0.0, 0.0, 0.0);
    OutB[id] = float2(energy, sootDensity);
}
