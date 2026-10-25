#include "common.hlsli"

struct SceneInstance
{
    float4x4 Transform;
    float4 Color;
    float4 Params; // x = damage, y = sway amplitude, z = phase, w = type (0 building, 1 vehicle, 2 tree, 3 rubble)
    float4 State;  // x = burning
    float4 TextureOrigin;
    float4 TextureScale;
};

StructuredBuffer<SceneInstance> Instances : register(t0);
// Per-chunk data for the Voronoi rubble; indexed by the vertex chunk id.
StructuredBuffer<SceneInstance> ChunkInstances : register(t1);

cbuffer ShadowConstants : register(b2)
{
    float4x4 LightViewProj[3];
    float4x4 StaticLightViewProj;
    float4 CascadeSplits;      // far distance of each cascade
    float4 CascadeTexel;       // 1 / shadow map size
    float4 CascadeWorldTexel;  // world size of one texel
    float4 StaticTexel;        // x = 1/size, y = world texel, z = depth bias
};

cbuffer ShadowCascade : register(b3)
{
    float4x4 ShadowViewProj;
};

Texture2D<float> ShadowMap0 : register(t2);
Texture2D<float> ShadowMap1 : register(t3);
Texture2D<float> ShadowMap2 : register(t4);
Texture2D<float> StaticShadowMap : register(t5);
SamplerComparisonState ShadowSampler : register(s2);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float3 Color : COLOR;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float3 Color : TEXCOORD2;
    float4 Params : TEXCOORD3;
    float4 State : TEXCOORD4;
    float3 LocalPosition : TEXCOORD5;
    float4 TextureOrigin : TEXCOORD6;
    float4 TextureScale : TEXCOORD7;
};

float3 ApplySway(float3 local, SceneInstance instance)
{
    if (instance.Params.w > 1.5 && instance.Params.y > 0.0)
    {
        float sway = sin(Misc3.x * 1.6 + instance.Params.z);
        local.x += sway * instance.Params.y * local.y;
        local.z += cos(Misc3.x * 1.3 + instance.Params.z) * instance.Params.y * local.y;
    }
    return local;
}

VSOutput VSScene(VSInput input, uint instanceID : SV_InstanceID)
{
    SceneInstance instance = Instances[instanceID];
    const float3 local = ApplySway(input.Position, instance);
    const float4 world = mul(instance.Transform, float4(local, 1.0));

    VSOutput output;
    output.Position = mul(ViewProj, world);
    output.WorldPosition = world.xyz;
    output.Normal = mul((float3x3)instance.Transform, input.Normal);
    output.Color = input.Color * instance.Color.rgb;
    output.Params = instance.Params;
    output.State = instance.State;
    output.LocalPosition = input.Position;
    output.TextureOrigin = instance.TextureOrigin;
    output.TextureScale = instance.TextureScale;
    return output;
}

// Depth-only pass used for the directional shadow map.
float4 VSShadow(VSInput input, uint instanceID : SV_InstanceID) : SV_Position
{
    SceneInstance instance = Instances[instanceID];
    const float3 local = ApplySway(input.Position, instance);
    const float4 world = mul(instance.Transform, float4(local, 1.0));
    return mul(ShadowViewProj, world);
}

// Voronoi rubble: the vertices are in building space and each one names its
// chunk. The chunk transform is the rigid motion relative to the fracture pose,
// so intact chunks stay exactly where they were cut from the building.
struct VSChunkInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    uint ChunkID : CHUNKID;
};

VSOutput VSChunk(VSChunkInput input)
{
    SceneInstance instance = ChunkInstances[input.ChunkID];
    const float4 world = mul(instance.Transform, float4(input.Position, 1.0));

    VSOutput output;
    output.Position = mul(ViewProj, world);
    output.WorldPosition = world.xyz;
    output.Normal = mul((float3x3)instance.Transform, input.Normal);
    output.Color = instance.Color.rgb;
    output.Params = instance.Params;
    output.State = instance.State;
    output.LocalPosition = input.Position;
    output.TextureOrigin = instance.TextureOrigin;
    output.TextureScale = instance.TextureScale;
    return output;
}

float4 VSShadowChunk(VSChunkInput input) : SV_Position
{
    SceneInstance instance = ChunkInstances[input.ChunkID];
    const float4 world = mul(instance.Transform, float4(input.Position, 1.0));
    return mul(ShadowViewProj, world);
}

float SampleShadowCascade(Texture2D<float> map, float4x4 lightViewProj,
                          float texelUV, float texelWorld, float3 worldPosition,
                          float3 normal)
{
    const float3 offsetWorld = normal * texelWorld * 1.5;
    const float3 offsetPosition = worldPosition + offsetWorld;
    const float3 lightPosition = mul(lightViewProj, float4(offsetPosition, 1.0)).xyz;
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

float SampleStaticShadow(float3 worldPosition, float3 normal)
{
    const float3 offsetWorld = normal * StaticTexel.y * 1.5;
    const float3 offsetPosition = worldPosition + offsetWorld;
    const float3 lightPosition =
        mul(StaticLightViewProj, float4(offsetPosition, 1.0)).xyz;
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

float SampleShadowDynamic(float3 worldPosition, float3 normal)
{
    const float distance = length(worldPosition - CameraPosition.xyz);
    const float blendWidth = 0.1;

    if (distance <= CascadeSplits.x)
    {
        const float s0 = SampleShadowCascade(ShadowMap0, LightViewProj[0],
            CascadeTexel.x, CascadeWorldTexel.x, worldPosition, normal);
        const float t = smoothstep(1.0 - blendWidth, 1.0,
            distance / max(CascadeSplits.x, 1e-3));
        if (t > 0.0)
        {
            const float s1 = SampleShadowCascade(ShadowMap1, LightViewProj[1],
                CascadeTexel.y, CascadeWorldTexel.y, worldPosition, normal);
            return lerp(s0, s1, t);
        }
        return s0;
    }
    if (distance <= CascadeSplits.y)
    {
        const float s1 = SampleShadowCascade(ShadowMap1, LightViewProj[1],
            CascadeTexel.y, CascadeWorldTexel.y, worldPosition, normal);
        const float t = smoothstep(1.0 - blendWidth, 1.0,
            distance / max(CascadeSplits.y, 1e-3));
        if (t > 0.0)
        {
            const float s2 = SampleShadowCascade(ShadowMap2, LightViewProj[2],
                CascadeTexel.z, CascadeWorldTexel.z, worldPosition, normal);
            return lerp(s1, s2, t);
        }
        return s1;
    }
    return SampleShadowCascade(ShadowMap2, LightViewProj[2], CascadeTexel.z,
                               CascadeWorldTexel.z, worldPosition, normal);
}

float SampleShadow(float3 worldPosition, float3 normal)
{
    // Lit only when neither the cached buildings nor the dynamic casters
    // occlude the fragment.
    return min(SampleShadowDynamic(worldPosition, normal),
               SampleStaticShadow(worldPosition, normal));
}

float4 PSScene(VSOutput input) : SV_Target
{
    float3 normal = normalize(input.Normal);
    float3 sunDirection = normalize(SunDirection.xyz);

    float3 baseColor = input.Color;
    float sunAmount = saturate(dot(normal, sunDirection));

    float3 toCamera = input.WorldPosition - CameraPosition.xyz;
    float viewDistance = length(toCamera);

    // Level of detail: fade the procedural facade out with distance so that
    // far buildings do not shimmer and their pixels stay cheap.
    float detailFade = 1.0 - saturate((viewDistance - 150.0) / 150.0);

    // The mesh-local position maps back to the original building space, so
    // rubble keeps the facade of the part it broke off from.
    const float3 facadePos = input.TextureOrigin.xyz + input.LocalPosition * input.TextureScale.xyz;

    // Procedural facade: windows on the vertical faces, darker roof on top.
    // Buildings (0) and rubble (3) share it; vehicles and trees do not.
    if (input.Params.w < 0.5 || input.Params.w > 2.5)
    {
        if (abs(normal.y) < 0.5)
        {
            float horizontal = abs(normal.x) > 0.5 ? facadePos.z : facadePos.x;
            float floorCell = frac(facadePos.y / 3.2);
            float bayCell = frac(horizontal / 2.6);
            if (floorCell > 0.22 && floorCell < 0.82 && bayCell > 0.18 && bayCell < 0.82)
            {
                baseColor = lerp(baseColor, float3(0.06, 0.10, 0.16), detailFade);
                sunAmount = lerp(sunAmount, 0.12, detailFade);
            }
        }
        else if (normal.y > 0.5)
        {
            baseColor *= 0.62;
        }
    }

    sunAmount *= SampleShadow(input.WorldPosition, normal);

    // Blast damage is an overlay: irregular char and cracks over the intact
    // facade, never a flat recolour.
    float damage = saturate(input.Params.x);
    if (damage > 0.01)
    {
        const float crackNoise = Fbm3(float3(facadePos.x * 0.55, facadePos.y * 0.35,
                                             facadePos.z * 0.55));
        const float crackMask = smoothstep(0.42, 0.60, crackNoise);
        const float soot = Fbm3(facadePos * 0.12 + float3(3.1, 1.7, 5.3));
        const float charAmount = damage * (0.22 + 0.45 * crackMask + 0.20 * soot);
        baseColor = lerp(baseColor, float3(0.055, 0.05, 0.045), saturate(charAmount));
    }

    float3 ambient = float3(0.16, 0.17, 0.20);
    float3 color = baseColor * (SunColor.rgb * sunAmount * 1.15 + ambient);

    // Burning objects glow with a flickering ember tint.
    float burning = saturate(input.State.x);
    float ember = 0.35 + 0.35 * saturate(sin(Misc3.x * 3.0 + input.WorldPosition.y * 0.6));
    color += float3(1.0, 0.35, 0.08) * burning * ember;

    // Distance haze, matching the ground and sky.
    float3 viewDirection = toCamera / max(viewDistance, 1e-4);
    float fog = 1.0 - exp(-max(viewDistance - 40.0, 0.0) * 0.0022);
    color = lerp(color, SkyColor(viewDirection), saturate(fog));

    return float4(color, 1.0);
}
