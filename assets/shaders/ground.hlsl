#include "common.hlsli"

EX_VK_BINDING(9) Texture2D<float> SceneDepth : register(t1);
EX_VK_BINDING(10) Texture2D<float> ShadowMap0 : register(t2);
EX_VK_BINDING(11) Texture2D<float> ShadowMap1 : register(t3);
EX_VK_BINDING(12) Texture2D<float> ShadowMap2 : register(t4);
EX_VK_BINDING(13) Texture2D<float> StaticShadowMap : register(t5);
EX_VK_BINDING(14) Texture3D<float4> VolumeTexture : register(t6);
EX_VK_BINDING(18) SamplerComparisonState ShadowSampler : register(s2);
EX_VK_BINDING(19) SamplerState VolumeSampler : register(s3);

EX_VK_BINDING(2) cbuffer ShadowConstants : register(b2)
{
    float4x4 LightViewProj[3];
    float4x4 StaticLightViewProj;
    float4 CascadeSplits;
    float4 CascadeTexel;
    float4 CascadeWorldTexel;
    float4 StaticTexel;
};

// Renders the infinite ground plane as a fullscreen ray/plane intersection and
// writes the correct hardware depth.

float SampleGroundCascade(Texture2D<float> map, float4x4 lightViewProj,
                          float texelUV, float texelWorld, float3 worldPosition)
{
    const float3 offsetPosition = worldPosition + float3(0.0, texelWorld * 1.5, 0.0);
    const float4 lightPosition = mul(lightViewProj, float4(offsetPosition, 1.0));
    const float2 uv = float2(0.5 + lightPosition.x * 0.5, 0.5 - lightPosition.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }
    const float depth = lightPosition.z - 0.0004;

    float shadow = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            shadow += map.SampleCmpLevelZero(
                ShadowSampler, uv + float2(x, y) * texelUV, depth);
        }
    }
    return shadow / 25.0;
}

float SampleGroundStatic(float3 worldPosition)
{
    const float3 offsetPosition =
        worldPosition + float3(0.0, StaticTexel.y * 1.5, 0.0);
    const float4 lightPosition =
        mul(StaticLightViewProj, float4(offsetPosition, 1.0));
    const float2 uv = float2(0.5 + lightPosition.x * 0.5,
                             0.5 - lightPosition.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }
    const float depth = lightPosition.z - StaticTexel.z;

    float shadow = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            shadow += StaticShadowMap.SampleCmpLevelZero(
                ShadowSampler, uv + float2(x, y) * StaticTexel.x, depth);
        }
    }
    return shadow / 9.0;
}

float SampleGroundDynamic(float3 worldPosition)
{
    const float distance = length(worldPosition - CameraPosition.xyz);
    const float blendWidth = 0.1;
    if (distance <= CascadeSplits.x)
    {
        const float s0 = SampleGroundCascade(ShadowMap0, LightViewProj[0],
            CascadeTexel.x, CascadeWorldTexel.x, worldPosition);
        const float t = smoothstep(1.0 - blendWidth, 1.0,
            distance / max(CascadeSplits.x, 1e-3));
        if (t > 0.0)
        {
            const float s1 = SampleGroundCascade(ShadowMap1, LightViewProj[1],
                CascadeTexel.y, CascadeWorldTexel.y, worldPosition);
            return lerp(s0, s1, t);
        }
        return s0;
    }
    if (distance <= CascadeSplits.y)
    {
        const float s1 = SampleGroundCascade(ShadowMap1, LightViewProj[1],
            CascadeTexel.y, CascadeWorldTexel.y, worldPosition);
        const float t = smoothstep(1.0 - blendWidth, 1.0,
            distance / max(CascadeSplits.y, 1e-3));
        if (t > 0.0)
        {
            const float s2 = SampleGroundCascade(ShadowMap2, LightViewProj[2],
                CascadeTexel.z, CascadeWorldTexel.z, worldPosition);
            return lerp(s1, s2, t);
        }
        return s1;
    }
    return SampleGroundCascade(ShadowMap2, LightViewProj[2], CascadeTexel.z,
                               CascadeWorldTexel.z, worldPosition);
}

// 5x5 PCF against the blended cascades and the cached static map.
float SampleGroundShadow(float3 worldPosition)
{
    return min(SampleGroundDynamic(worldPosition),
               SampleGroundStatic(worldPosition));
}

// Volumetric shadow of the smoke/fireball cloud: march towards the sun and
// accumulate the smoke optical depth. Cheap because most ground pixels miss
// the volume box entirely.
float SmokeShadow(float3 worldPosition)
{
    const float3 sunDir = normalize(SunDirection.xyz);
    float2 range = RayBoxRange(worldPosition, sunDir, VolumeMin.xyz, VolumeMax.xyz);
    range.x = max(range.x, 0.0);
    if (range.y <= range.x)
    {
        return 1.0;
    }

    const int steps = 8;
    const float stepSize = (range.y - range.x) / float(steps);
    float t = range.x + stepSize * 0.5;
    float opticalDepth = 0.0;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        const float3 position = worldPosition + sunDir * t;
        const float3 uvw = (position - VolumeMin.xyz) /
                           max(VolumeMax.xyz - VolumeMin.xyz, 1e-4);
        const float density = VolumeTexture.SampleLevel(VolumeSampler, uvw, 0).r;
        opticalDepth += density * VolumeMin.w * stepSize;
        t += stepSize;
    }
    return exp(-opticalDepth);
}

float TerrainHeight(float2 p)
{
    float detail = Fbm3(float3(p * 0.03, 1.7));
    float broad = Fbm3(float3(p * 0.004, 4.2));
    return (detail - 0.5) * 1.6 + (broad - 0.5) * 4.0;
}

struct GroundOutput
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

GroundOutput PSGround(VSOut input)
{
    GroundOutput output;
    float3 origin = CameraPosition.xyz;
    float3 direction = RayDirection(input.uv);

    // Ground plane at y = 0.
    if (abs(direction.y) < 1e-5 || origin.y * direction.y >= 0.0)
    {
        discard;
    }

    float distanceToPlane = -origin.y / direction.y;
    if (distanceToPlane <= 0.0 || distanceToPlane > RangeParams.y * 4.0)
    {
        discard;
    }

    float3 world = origin + direction * distanceToPlane;
    float2 p = world.xz;
    float radius = length(p);

    // --- Terrain colour -----------------------------------------------------
    float large = Fbm3(float3(p * 0.006, 2.0));
    float medium = Fbm3(float3(p * 0.045, 5.0));
    float fine = ValueNoise3(float3(p * 0.35, 9.0));

    float3 earth = lerp(float3(0.105, 0.082, 0.062), float3(0.185, 0.150, 0.108), medium);
    float grass = saturate(large * 1.7 - 0.55);
    earth = lerp(earth, float3(0.075, 0.090, 0.048), grass * 0.75);
    earth *= 0.85 + 0.25 * fine;

    // --- Blast effects ------------------------------------------------------
    float scorch = 1.0 - smoothstep(Misc.x * 0.15, Misc.x * 1.15, radius);
    earth *= lerp(1.0, 0.16, scorch);

    float ring = exp(-pow((radius - Misc.y) / max(Misc.y * 0.018, 0.8), 2.0));
    earth = lerp(earth, float3(0.20, 0.17, 0.14), ring * 0.65);

    if (Misc.z > 1.0)
    {
        float dustWidth = max(Misc.z * 0.06, 1.5);
        float dustRing = exp(-pow((radius - Misc.z) / dustWidth, 2.0));
        float dustNoise = 0.55 + 0.45 * Fbm3(float3(p * 0.12, Misc3.x * 0.4));
        earth = lerp(earth, float3(0.46, 0.39, 0.31), dustRing * dustNoise * 0.8);
    }

    // --- Grid ---------------------------------------------------------------
    float spacing = GridParams.x;
    float2 gridCoord = p / spacing;
    float2 gridDerivative = max(fwidth(gridCoord), 1e-5);
    float2 gridDistance = abs(frac(gridCoord - 0.5) - 0.5) / gridDerivative;
    float gridLine = 1.0 - saturate(min(gridDistance.x, gridDistance.y) * max(GridParams.y, 0.5));
    gridLine *= saturate(1.0 - radius / max(GridParams.w, 1.0)) * GridParams.z;
    earth = lerp(earth, float3(0.55, 0.52, 0.46), gridLine);

    // --- Lighting -----------------------------------------------------------
    float3 normal = float3(0.0, 1.0, 0.0);
    float sunAmount = saturate(dot(normal, SunDirection.xyz));
    sunAmount *= SampleGroundShadow(world);
    sunAmount *= SmokeShadow(world);
    float3 lighting = SunColor.rgb * sunAmount * 0.35 + float3(0.16, 0.17, 0.20);
    float3 color = earth * lighting;

    // Fireball illuminates the ground: inverse-square falloff with a soft core.
    float3 toFireball = Fireball.xyz - world;
    float fireDistanceSq = dot(toFireball, toFireball);
    float fireRadius = max(Misc2.z, 0.2);
    float fireLight = Fireball.w * 2.0 * fireRadius * fireRadius /
                      (fireDistanceSq + fireRadius * fireRadius * 1.2);
    color += earth * fireLight * float3(1.0, 0.48, 0.16) * 0.9;

    // --- Distance haze ------------------------------------------------------
    float fog = 1.0 - exp(-max(distanceToPlane - 60.0, 0.0) * 0.0011);
    color = lerp(color, SkyColor(direction), saturate(fog));

    float4 clip = mul(ViewProj, float4(world, 1.0));
    output.color = float4(color, 1.0);
    output.depth = clip.z / clip.w;
    return output;
}
