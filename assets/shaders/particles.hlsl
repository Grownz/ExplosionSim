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

EX_VK_BINDING(8) StructuredBuffer<Instance> Instances : register(t0);
EX_VK_BINDING(9) Texture2D<float> SceneDepth : register(t1);
EX_VK_BINDING(16) SamplerState LinearSampler : register(s0);

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
    float3 worldPosition : TEXCOORD7;
    float streak : TEXCOORD8;
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
    output.uv = rotated;
    output.color = instance.color;
    output.opacity = instance.opacity;
    output.variation = instance.variation;
    output.viewDistance = length(instance.position - CameraPosition.xyz);
    output.size = max(size, 0.02);
    output.intensity = instance.padding.x > 0.0 ? instance.padding.x : 1.0;
    // Fragment world position, used to keep the flame pattern world-anchored.
    output.worldPosition = world;
    output.streak = 0.0;

    if (instance.padding.y > 0.5)
    {
        // Edge ember: a short screen-space line along the velocity direction.
        // The colour slot carries the direction, the size is the length.
        const float4 clipStart = mul(ViewProj, float4(instance.position, 1.0));
        const float4 clipEnd =
            mul(ViewProj, float4(instance.position + instance.color * instance.size, 1.0));
        const float2 start = clipStart.xy / max(clipStart.w, 1e-4);
        const float2 end = clipEnd.xy / max(clipEnd.w, 1e-4);

        // Build the quad in pixels so the width stays constant at any aspect.
        const float2 sizePixels = ScreenParams.xy;
        const float2 startPixels = (start * 0.5 + 0.5) * sizePixels;
        const float2 endPixels = (end * 0.5 + 0.5) * sizePixels;
        const float2 delta = endPixels - startPixels;
        const float lengthPixels = max(length(delta), 4.0);
        const float2 axis = length(delta) > 0.01 ? delta / length(delta)
                                                 : float2(0.0, 1.0);
        const float2 perpendicular = float2(-axis.y, axis.x);
        const float2 pixels = startPixels + axis * corner.y * lengthPixels * 0.5 +
                              perpendicular * corner.x * 1.6;
        const float2 ndc = pixels * ScreenParams.zw * 2.0 - 1.0;

        output.position = float4(ndc, clipStart.z / max(clipStart.w, 1e-4), 1.0);
        output.streak = 1.0;
    }
    else
    {
        output.position = mul(ViewProj, float4(world, 1.0));
    }
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
    if (input.streak > 0.5)
    {
        // Edge line burst: bright core, soft sides and a fade towards both
        // ends so the streak does not read as a hard rectangle.
        const float across = saturate(1.0 - abs(input.uv.x));
        const float along = abs(input.uv.y);
        const float body = across * across * smoothstep(1.0, 0.45, along);
        const float3 color = lerp(float3(1.0, 0.30, 0.04), float3(1.0, 0.86, 0.62),
                                  smoothstep(0.0, 0.7, across) * (1.0 - along));
        const float fade = SoftDepthFade(input);
        return float4(color * input.intensity * input.opacity * body * fade * 2.4, 1.0);
    }

    float radiusSquared = dot(input.uv, input.uv);
    if (radiusSquared > 1.0)
    {
        discard;
    }

    float falloff = exp(-radiusSquared * 3.2) * (1.0 - radiusSquared);
    float fade = SoftDepthFade(input);
    float3 color = input.color * falloff * input.opacity * 2.6 * input.intensity;
    return float4(color * fade, 1.0);
}

// GPU fire: procedural flame tongues on an additive billboard. Two fbm
// lookups shape the silhouette and the heat gradient, so the CPU only has to
// advance the emitter positions. The result reads as a turbulent flame while
// staying a single instanced draw call.
float4 PSFire(ParticleOutput input) : SV_Target
{
    float radiusSquared = dot(input.uv, input.uv);
    if (radiusSquared > 1.0)
    {
        discard;
    }

    const float time = Misc3.x;
    const float seed = input.variation * 43.0;
    const float2 uv = input.uv;

    // Flames narrow towards the top of the billboard.
    const float height = saturate(uv.y * 0.5 + 0.5);
    const float width = lerp(1.0, 0.32, height);

    // World-space turbulence: the billboard rotates with the camera, but the
    // flame pattern is sampled in world space, so it does not swim with the view.
    float3 q = input.worldPosition * (2.0 / max(input.size, 0.3)) +
               float3(seed, time * 2.6, 0.0);
    q.x += sin(time * 1.9 + seed) * 0.22;
    const float turbulence = Fbm3(q);
    const float detail = Fbm3(q * 2.7 + float3(0.0, time * 4.2, 1.7));

    const float body = saturate(1.0 - length(float2(uv.x / width, uv.y)));
    float flame = body + (turbulence - 0.5) * 0.75 + (detail - 0.5) * 0.22;
    flame = smoothstep(0.10, 0.92, flame) * (1.0 - height * 0.35);

    // Heat ramp: deep red rim, orange body, white-hot core.
    float heat = saturate(body * 1.15 + (turbulence - 0.5) * 0.55 +
                          (1.0 - height) * 0.22);
    float3 color = lerp(float3(0.42, 0.045, 0.01), float3(1.0, 0.36, 0.05),
                        smoothstep(0.10, 0.62, heat));
    color = lerp(color, float3(1.0, 0.86, 0.58), smoothstep(0.62, 0.96, heat));
    color = lerp(input.color, color, 0.7);

    const float flicker = 0.82 + 0.36 * Fbm3(float3(seed, time * 3.1, 0.0));
    const float fade = SoftDepthFade(input);
    const float intensity = flame * input.opacity * input.intensity *
                            flicker * 5.5;

    // Fade out billboards that are almost at the camera so a single flame
    // cannot fill the screen.
    const float cameraFade = smoothstep(0.4, 2.0, input.viewDistance);
    return float4(color * intensity * fade * cameraFade, 1.0);
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
