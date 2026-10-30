#include "common.hlsli"

EX_VK_BINDING(8) Texture2D<float4> SceneTexture : register(t0);
EX_VK_BINDING(9) Texture2D<float4> BloomTexture : register(t1);
EX_VK_BINDING(16) SamplerState LinearSampler : register(s0);

float3 ACESFilm(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 LinearToSrgb(float3 color)
{
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, color));
}

// Final HDR composite: bloom, exposure, filmic tonemap, vignette and dithering.
float4 PSComposite(VSOut input) : SV_Target
{
    float3 hdr = SceneTexture.SampleLevel(LinearSampler, input.uv, 0).rgb;
    float3 bloom = BloomTexture.SampleLevel(LinearSampler, input.uv, 0).rgb;

    float3 color = (hdr + bloom * BloomParams.w) * BloomParams.z;
    color = ACESFilm(color);

    float2 centered = input.uv * 2.0 - 1.0;
    float vignette = saturate(1.0 - dot(centered, centered) * 0.10);
    color *= vignette;

    color = LinearToSrgb(saturate(color));
    float dither = (Hash12(input.position.xy) - 0.5) / 255.0;
    return float4(saturate(color + dither), 1.0);
}
