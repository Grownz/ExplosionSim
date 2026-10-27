#include "euler_common.hlsli"

// Resamples the conservative state onto a larger domain with the same voxel
// count. Cells outside the old domain are filled with ambient air. Used for the
// high-fidelity auto-scale: the box grows, dx grows, the CFL step grows and the
// effective accuracy drops while the cost per step stays constant.
[numthreads(8, 8, 8)]
void CSRescale(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= (uint3)Grid.xyz))
    {
        return;
    }

    if (!IsActiveCell(int3(id)))
    {
        OutA[id] = float4(Ambient.x, 0.0, 0.0, 0.0);
        OutB[id] = float2(Ambient.y / (Ambient.w - 1.0), 0.0);
        return;
    }

    float3 world = DomainMin.xyz + (float3(id) + 0.5) * DomainMin.w;
    float3 oldSize = OldDomainMax.xyz - OldDomainMin.xyz;
    float3 uv = (world - OldDomainMin.xyz) / max(oldSize, 1e-5);

    float4 a;
    float2 b;

    if (all(uv >= 0.0) && all(uv <= 1.0))
    {
        a = StateA.SampleLevel(LinearSampler, uv, 0);
        b = StateB.SampleLevel(LinearSampler, uv, 0);
    }
    else
    {
        a = float4(Ambient.x, 0.0, 0.0, 0.0);
        b = float2(Ambient.y / (Ambient.w - 1.0), 0.0);
    }

    Sanitise(a, b);

    OutA[id] = a;
    OutB[id] = b;
}
