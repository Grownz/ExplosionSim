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
    float3 world = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;

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

    // Sub-grid cap closure. The unresolved vortex ring is replaced by a
    // toroidal force around the analytic cap: outward at the top, downward
    // outside, inward at the bottom and upward inside. This spins the cap into
    // a mushroom instead of letting it rise as a column.
    float3 radial = float3(world.x, 0.0, world.z);
    float radialLength = length(radial);
    float3 radialDirection = radial / max(radialLength, 1e-3);
    float targetRadius = max(Extra.x, 2.0 * DomainMin.w);
    float targetCenterY = Extra.y;
    float dr = radialLength - targetRadius;
    float dy = world.y - targetCenterY;
    float ringDistance = sqrt(dr * dr + dy * dy);
    float ringWidth = max(0.45 * targetRadius, 2.0 * DomainMin.w);
    float ring = exp(-pow(ringDistance / ringWidth, 2.0));
    float2 tangent = float2(dy, -dr) / max(ringDistance, 1e-3);
    float strength = 25.0 * ring * plumeMask;
    float impulse = rho * strength * DomainMax.w;
    a.y += radialDirection.x * tangent.x * impulse;
    a.z += tangent.y * impulse;
    a.w += radialDirection.z * tangent.x * impulse;
    energy += impulse * (tangent.x * dot(radialDirection, velocity) +
                         tangent.y * velocity.y);

    float above =
        saturate((world.y - (targetCenterY + targetRadius)) / targetRadius);
    float verticalDamping = 2.0 * above;
    a.z -= rho * max(velocity.y, 0.0) * verticalDamping * DomainMax.w;
    energy -= 0.5 * rho * max(velocity.y, 0.0) * max(velocity.y, 0.0) *
              verticalDamping * DomainMax.w;

    // Radiative cooling of hot soot.
    energy -= Physics.z * soot * max(temperature - Ambient.z, 0.0) * DomainMax.w;

    b.x = energy;
    b.y = soot;

    // --- Open-boundary sponge --------------------------------------------
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
