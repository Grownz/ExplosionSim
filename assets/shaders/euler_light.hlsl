#include "euler_common.hlsli"

// Precomputes the sun transmittance for every voxel of a small 3D texture. The
// volume raymarcher samples this view-independent volume instead of marching
// towards the sun once per ray (which made the shading depend on the camera).
EX_VK_BINDING(10) Texture3D<float4> RenderVolume : register(t2);
EX_VK_BINDING(25) RWTexture3D<float> LightVolume : register(u2);

float2 LightRayBoxRange(float3 origin, float3 direction, float3 boxMin, float3 boxMax)
{
    float3 safeDirection = direction + step(abs(direction), 1e-6) * 1e-6 *
                           sign(direction + 1e-9);
    float3 invDirection = 1.0 / safeDirection;
    float3 t0 = (boxMin - origin) * invDirection;
    float3 t1 = (boxMax - origin) * invDirection;
    float3 tMin = min(t0, t1);
    float3 tMax = max(t0, t1);
    return float2(max(max(tMin.x, tMin.y), tMin.z),
                  min(min(tMax.x, tMax.y), tMax.z));
}

[numthreads(4, 4, 4)]
void CSLight(uint3 id : SV_DispatchThreadID)
{
    uint3 size;
    LightVolume.GetDimensions(size.x, size.y, size.z);
    if (any(id >= size))
    {
        return;
    }

    const float3 uvw = (float3(id) + 0.5) / float3(size);
    const float3 world = DomainMin.xyz + uvw * (DomainMax.xyz - DomainMin.xyz);
    const float3 sunDir = normalize(SunDirection.xyz);

    float2 range = LightRayBoxRange(world, sunDir, DomainMin.xyz, DomainMax.xyz);
    range.x = max(range.x, 0.0);

    const int steps = max((int)LightParams.y, 1);
    const float stepSize = (range.y - range.x) / float(steps);
    float opticalDepth = 0.0;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float3 position = world + sunDir * (range.x + stepSize * (float(i) + 0.5));
        const float3 sampleUvw = (position - DomainMin.xyz) /
                                 max(DomainMax.xyz - DomainMin.xyz, 1e-4);
        const float density = RenderVolume.SampleLevel(LinearSampler, sampleUvw, 0).r;
        opticalDepth += density * LightParams.x * stepSize;
    }

    LightVolume[id] = exp(-opticalDepth);
}
