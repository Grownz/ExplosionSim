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

float3 FacadeMaterial(VSOutput input, float3 baseColor, float detailFade,
                      inout float sunAmount)
{
    const float3 n = normalize(input.Normal);
    const float3 p = input.TextureOrigin.xyz + input.LocalPosition * input.TextureScale.xyz;
    const int style = int(input.TextureScale.w + 0.5) % 4;

    if (n.y > 0.5)
    {
        const float gravel = Fbm3(p * 1.7);
        baseColor *= 0.52 + 0.14 * gravel;
        const float vent = smoothstep(0.72, 0.92, Fbm3(p * 0.55 + 7.1));
        baseColor = lerp(baseColor, float3(0.30, 0.31, 0.33), vent * detailFade * 0.7);
        return baseColor;
    }
    if (n.y < -0.5)
    {
        return baseColor * 0.45;
    }

    const float u = abs(n.x) > 0.5 ? p.z : p.x;
    const float v = p.y;
    const float2 uv = float2(u, v);
    float3 color = baseColor;

    if (style == 0)
    {
        // Glass curtain wall with mullions and lit offices.
        const float2 cellSize = float2(2.2, 3.4);
        const float2 cell = floor(uv / cellSize);
        const float2 f = frac(uv / cellSize);
        const float frame = saturate(
            (1.0 - smoothstep(0.0, 0.05, min(f.x, 1.0 - f.x))) +
            (1.0 - smoothstep(0.0, 0.07, min(f.y, 1.0 - f.y))));
        const float glass = step(0.08, f.x) * step(f.x, 0.92) *
                            step(0.18, f.y) * step(f.y, 0.88);
        const float3 view = normalize(CameraPosition.xyz - input.WorldPosition);
        const float3 sky = SkyColor(reflect(-view, n));
        const float lit = step(0.72, Hash12(cell + style * 13.0));
        float3 glassColor = lerp(float3(0.035, 0.075, 0.13), sky * 0.55, 0.55);
        glassColor = lerp(glassColor, float3(1.0, 0.72, 0.38) * 1.6, lit * 0.55);
        color = lerp(baseColor, glassColor, glass * detailFade);
        color = lerp(color, float3(0.55, 0.56, 0.58), frame * detailFade);
        sunAmount *= lerp(1.0, 0.35, glass * detailFade);
    }
    else if (style == 1)
    {
        // Brick facade with mortar, punched windows and sills.
        const float2 brickSize = float2(0.26, 0.085);
        const float2 brick = floor(uv / brickSize);
        const float2 bf = frac(uv / brickSize);
        const float mortar = step(bf.x, 0.06) + step(0.94, bf.x) +
                             step(bf.y, 0.10) + step(0.90, bf.y);
        const float shade = 0.78 + 0.35 * Hash12(brick + style * 7.0);
        float3 brickColor = baseColor * shade;
        brickColor = lerp(brickColor, float3(0.42, 0.40, 0.36),
                          saturate(mortar) * 0.65);
        const float2 winSize = float2(2.8, 3.3);
        const float2 wcell = floor(uv / winSize);
        const float2 wf = frac(uv / winSize);
        const float window = step(0.30, wf.x) * step(wf.x, 0.78) *
                             step(0.28, wf.y) * step(wf.y, 0.76);
        const float frame = saturate(
            (1.0 - smoothstep(0.0, 0.055, min(abs(wf.x - 0.30), abs(wf.x - 0.78)))) +
            (1.0 - smoothstep(0.0, 0.055, min(abs(wf.y - 0.28), abs(wf.y - 0.76)))));
        const float lit = step(0.78, Hash12(wcell + 3.0));
        const float3 windowColor =
            lerp(float3(0.04, 0.07, 0.10), float3(1.0, 0.70, 0.35) * 1.4, lit);
        color = lerp(brickColor, windowColor, window * detailFade);
        color = lerp(color, float3(0.68, 0.66, 0.60), frame * detailFade * 0.8);
        sunAmount *= lerp(1.0, 0.45, window * detailFade);
    }
    else if (style == 2)
    {
        // Concrete office: panel seams, stains and narrow windows.
        const float2 panel = float2(1.35, 1.15);
        const float2 pf = frac(uv / panel);
        const float seam = step(pf.x, 0.035) + step(0.965, pf.x) +
                           step(pf.y, 0.04) + step(0.96, pf.y);
        const float stain = Fbm3(float3(u * 0.35, v * 0.12, 1.7));
        color = baseColor * (0.82 + 0.20 * stain);
        color = lerp(color, color * 0.62, saturate(seam) * 0.8);
        const float2 wf = frac(uv / float2(2.4, 3.1));
        const float window = step(0.36, wf.x) * step(wf.x, 0.64) *
                             step(0.26, wf.y) * step(wf.y, 0.72);
        color = lerp(color, float3(0.045, 0.075, 0.10), window * detailFade);
        sunAmount *= lerp(1.0, 0.5, window * detailFade);
    }
    else
    {
        // Industrial: corrugated metal, rust streaks and high windows.
        const float corrugation = frac(u / 0.16);
        const float rib = 1.0 - smoothstep(0.0, 0.16,
                                           min(corrugation, 1.0 - corrugation));
        color = baseColor * (0.72 + 0.22 * rib);
        const float rust = smoothstep(0.55, 0.85, Fbm3(float3(u * 0.9, v * 0.22, 5.2)));
        color = lerp(color, float3(0.28, 0.13, 0.06), rust * 0.55);
        const float2 wf = frac(uv / float2(3.2, 2.4));
        const float window = step(0.30, wf.x) * step(wf.x, 0.70) *
                             step(0.62, wf.y) * step(wf.y, 0.86);
        color = lerp(color, float3(0.05, 0.08, 0.11), window * detailFade);
        sunAmount *= lerp(1.0, 0.55, window * detailFade);
    }
    return lerp(baseColor, color, detailFade);
}

float3 VehicleMaterial(VSOutput input, float3 baseColor, float detailFade)
{
    const float3 n = normalize(input.Normal);
    const float3 lp = input.LocalPosition;
    const int style = int(input.TextureScale.w + 0.5) % 3;
    const float paintNoise = Fbm3(lp * 3.5 + float3(style * 5.0, 0.0, 0.0));
    float3 color = baseColor * (0.82 + 0.28 * paintNoise);
    color = lerp(color, color * 1.25, saturate(n.y) * 0.35);

    const float side = step(abs(n.y), 0.55);
    float glass = 0.0;
    if (style == 0)
    {
        glass = step(0.58, lp.y) * step(lp.y, 0.98) * side * step(abs(lp.x), 0.82);
        glass = max(glass, step(0.62, lp.y) * step(lp.y, 1.0) *
                            step(0.55, abs(n.z)) * step(abs(lp.x), 0.75));
    }
    else if (style == 1)
    {
        glass = step(0.95, lp.y) * step(lp.y, 1.30) * side * step(0.8, lp.z);
        glass = max(glass, step(1.0, lp.y) * step(lp.y, 1.3) *
                            step(0.55, abs(n.z)) * step(abs(lp.x), 0.8));
    }
    else
    {
        glass = step(0.78, lp.y) * step(lp.y, 1.22) *
                step(0.55, abs(n.z)) * step(0.8, lp.z);
        glass = max(glass, step(0.8, lp.y) * step(lp.y, 1.2) * side *
                            step(0.8, lp.z));
    }
    const float3 view = normalize(CameraPosition.xyz - input.WorldPosition);
    const float3 glassColor = lerp(float3(0.025, 0.045, 0.07),
                                  SkyColor(reflect(-view, n)) * 0.5, 0.45);
    color = lerp(color, glassColor, glass * detailFade);

    // Lower body, bumpers and tires.
    color = lerp(color, color * 0.45, step(lp.y, 0.32) * detailFade);
    const float wheel = step(0.72, abs(lp.x)) * step(lp.y, 0.48) *
                        step(1.0, abs(lp.z));
    const float tread = 0.75 + 0.25 * step(0.5, frac(lp.z * 8.0));
    color = lerp(color, float3(0.035, 0.035, 0.04) * tread, wheel * detailFade);

    // Headlights and taillights on the front/rear faces.
    const float frontZ = style == 0 ? 1.9 : (style == 1 ? 2.2 : 2.0);
    const float rearZ = style == 0 ? 1.9 : (style == 1 ? 2.2 : 2.4);
    const float lampY = step(0.38, lp.y) * step(lp.y, 0.62);
    const float lampX = step(0.35, abs(lp.x)) * step(abs(lp.x), 0.80);
    const float lampFace = step(0.5, abs(n.z));
    const float head = step(frontZ, lp.z) * lampY * lampX * lampFace;
    const float tail = step(rearZ, -lp.z) * lampY * lampX * lampFace;
    color = lerp(color, float3(1.0, 0.92, 0.72) * 2.5, head * detailFade);
    color = lerp(color, float3(0.9, 0.06, 0.03) * 2.0, tail * detailFade);
    return color;
}

float3 TreeMaterial(VSOutput input, float3 baseColor, float detailFade)
{
    const float3 lp = input.LocalPosition;
    const float3 n = normalize(input.Normal);
    const float radial = length(lp.xz);
    const int style = int(input.TextureScale.w + 0.5) % 3;
    const float trunk = step(lp.y, 0.42) * step(radial, 0.10);
    const float barkNoise = Fbm3(float3(lp.x * 24.0, lp.y * 5.0, lp.z * 24.0));
    const float3 bark = lerp(float3(0.16, 0.10, 0.055),
                             float3(0.28, 0.19, 0.11), barkNoise);

    const float canopy = step(0.30, lp.y);
    const float clump = Fbm3(lp * 7.0 + float3(style * 4.0, 0.0, style * 2.0));
    const float3 leafA = baseColor * (0.72 + 0.42 * clump);
    const float3 leafB = float3(0.10, 0.24, 0.07) * (0.8 + 0.5 * clump);
    float3 leaves = lerp(leafA, leafB, 0.35 + 0.2 * style);
    leaves *= 0.75 + 0.35 * saturate(n.y);
    float3 color = lerp(baseColor, leaves, canopy * detailFade);
    color = lerp(color, bark, trunk * detailFade);
    return color;
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

    // High-resolution procedural materials. Buildings and rubble share the
    // facade styles; vehicles and trees have their own detail.
    if (input.Params.w < 0.5 || input.Params.w > 2.5)
    {
        baseColor = FacadeMaterial(input, baseColor, detailFade, sunAmount);
    }
    else if (input.Params.w < 1.5)
    {
        baseColor = VehicleMaterial(input, baseColor, detailFade);
    }
    else
    {
        baseColor = TreeMaterial(input, baseColor, detailFade);
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
