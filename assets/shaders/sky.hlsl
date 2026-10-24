#include "common.hlsli"

// Renders the procedural sky. Runs before any geometry and writes no depth.
float4 PSSky(VSOut input) : SV_Target
{
    float3 ray = RayDirection(input.uv);
    float3 color = SkyColor(ray);
    return float4(color, 1.0);
}
