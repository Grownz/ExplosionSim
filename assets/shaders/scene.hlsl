#include "common.hlsli"

struct SceneInstance
{
    float4x4 Transform;
    float4 Color;
    float4 Params; // x = damage, y = sway amplitude, z = phase, w = type (0 building, 1 vehicle, 2 tree, 3 rubble)
    float4 State;  // x = burning
};

StructuredBuffer<SceneInstance> Instances : register(t0);

cbuffer ShadowConstants : register(b2)
{
    float4x4 LightViewProj;
};

Texture2D<float> ShadowMap : register(t2);
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
    return output;
}

// Depth-only pass used for the directional shadow map.
float4 VSShadow(VSInput input, uint instanceID : SV_InstanceID) : SV_Position
{
    SceneInstance instance = Instances[instanceID];
    const float3 local = ApplySway(input.Position, instance);
    const float4 world = mul(instance.Transform, float4(local, 1.0));
    return mul(LightViewProj, world);
}

float SampleShadow(float3 worldPosition)
{
    const float3 lightPosition = mul(LightViewProj, float4(worldPosition, 1.0)).xyz;
    const float2 uv = float2(0.5 + lightPosition.x * 0.5, 0.5 - lightPosition.y * 0.5);
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

    // Procedural facade: windows on the vertical faces, darker roof on top.
    if (input.Params.w < 0.5)
    {
        if (abs(normal.y) < 0.5)
        {
            float horizontal = abs(normal.x) > 0.5 ? input.WorldPosition.z : input.WorldPosition.x;
            float floorCell = frac(input.WorldPosition.y / 3.2);
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

    sunAmount *= SampleShadow(input.WorldPosition);

    // Blast damage: darken and char.
    float damage = saturate(input.Params.x);
    baseColor = lerp(baseColor, float3(0.07, 0.065, 0.06), damage * 0.75);

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
