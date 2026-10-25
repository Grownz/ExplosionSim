#include "common.hlsli"

Texture2D<float> SceneDepth : register(t1);
Texture2D<float> ShadowMap : register(t2);
SamplerComparisonState ShadowSampler : register(s2);

cbuffer ShadowConstants : register(b2)
{
    float4x4 LightViewProj;
};

// Renders the infinite ground plane as a fullscreen ray/plane intersection and
// writes the correct hardware depth.

// 3x3 PCF against the directional shadow map, matching scene.hlsl.
float SampleGroundShadow(float3 worldPosition)
{
    const float4 lightPosition = mul(LightViewProj, float4(worldPosition, 1.0));
    const float2 uv = float2(0.5 + lightPosition.x * 0.5, 0.5 - lightPosition.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }
    const float depth = lightPosition.z - 0.0015;
    const float texel = 1.0 / 2048.0;

    float shadow = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            shadow += ShadowMap.SampleCmpLevelZero(
                ShadowSampler, uv + float2(x, y) * texel, depth);
        }
    }
    return shadow / 9.0;
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
