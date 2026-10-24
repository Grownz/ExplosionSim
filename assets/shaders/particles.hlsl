#include "common.hlsli"

struct Instance
{
    float3 position;
    float size;
    float3 color;
    float opacity;
    float rotation;
    float variation;
    float2 padding;
};

StructuredBuffer<Instance> Instances : register(t0);
Texture2D<float> SceneDepth : register(t1);
SamplerState LinearSampler : register(s0);

struct ParticleOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 color : TEXCOORD1;
    float opacity : TEXCOORD2;
    float variation : TEXCOORD3;
    float viewDistance : TEXCOORD4;
    float size : TEXCOORD5;
    float intensity : TEXCOORD6;
};

ParticleOutput VSParticle(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    Instance instance = Instances[instanceID];

    // Two triangles forming a quad: (-1,-1) (1,-1) (-1,1) / (-1,1) (1,-1) (1,1).
    float2 corner;
    corner.x = (vertexID == 1 || vertexID == 4 || vertexID == 5) ? 1.0 : -1.0;
    corner.y = (vertexID == 2 || vertexID == 3 || vertexID == 5) ? 1.0 : -1.0;

    float angle = frac(instance.rotation) * 6.2831853;
    float s = sin(angle);
    float c = cos(angle);
    float2 rotated = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);

    float size = instance.size * RangeParams.w;
    float3 world = instance.position +
                   (CameraRight.xyz * rotated.x + CameraUp.xyz * rotated.y) * max(size, 0.02);

    ParticleOutput output;
    output.position = mul(ViewProj, float4(world, 1.0));
    output.uv = rotated;
    output.color = instance.color;
    output.opacity = instance.opacity;
    output.variation = instance.variation;
    output.viewDistance = length(instance.position - CameraPosition.xyz);
    output.size = max(size, 0.02);
    output.intensity = instance.padding.x > 0.0 ? instance.padding.x : 1.0;
    return output;
}

float SoftDepthFade(ParticleOutput input)
{
    float sceneDepth = SceneDepth.Load(int3(input.position.xy, 0));
    float3 scenePosition = WorldFromUV(input.position.xy * ScreenParams.zw, sceneDepth);
    float sceneDistance = length(scenePosition - CameraPosition.xyz);
    return saturate((sceneDistance - input.viewDistance) / max(input.size * 1.4, 1.0));
}

float3 ParticleLighting(float2 uv)
{
    float2 lightDirection = normalize(float2(dot(SunDirection.xyz, CameraRight.xyz),
                                             dot(SunDirection.xyz, CameraUp.xyz)) +
                                      float2(1e-4, 1e-4));
    float lambert = saturate(dot(normalize(float3(uv, 0.35)), float3(lightDirection, 0.75)));
    return float3(0.42, 0.44, 0.48) + SunColor.rgb * lambert * 0.55;
}

// Glowing sparks, hot debris and flame tongues (additive).
float4 PSAdditive(ParticleOutput input) : SV_Target
{
    float radiusSquared = dot(input.uv, input.uv);
    if (radiusSquared > 1.0)
    {
        discard;
    }

    float falloff = exp(-radiusSquared * 3.2) * (1.0 - radiusSquared);
    float fade = SoftDepthFade(input);
    float3 color = input.color * falloff * input.opacity * 3.2 * input.intensity;
    return float4(color * fade, 1.0);
}

// Smoke and dust puffs (alpha blended).
float4 PSAlpha(ParticleOutput input) : SV_Target
{
    float radiusSquared = dot(input.uv, input.uv);
    if (radiusSquared > 1.0)
    {
        discard;
    }

    float edge = saturate(1.0 - radiusSquared);
    float noise = Fbm3(float3(input.uv * 1.6, input.variation));
    float alpha = input.opacity * pow(edge, 1.6) * (0.45 + 0.75 * noise);
    alpha *= SoftDepthFade(input);

    if (alpha < 0.002)
    {
        discard;
    }

    float3 color = input.color * ParticleLighting(input.uv);
    return float4(color, saturate(alpha));
}
